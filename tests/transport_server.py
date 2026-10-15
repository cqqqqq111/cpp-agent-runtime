"""HTTP/1.1 localhost fixture with observable connection/request lifetimes."""
import http.server
import json
import socket
import threading
import time


class TransportServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, connection_delay=0):
        super().__init__(('127.0.0.1', 0), Handler)
        self.connection_delay = connection_delay
        self.lock = threading.Condition()
        self.connections = 0
        self.open_connections = 0
        self.requests = []
        self.active_requests = 0
        self.peak_requests = 0
        self.release_requests = threading.Event()
        self.thread = threading.Thread(target=self.serve_forever, daemon=True)

    @property
    def url(self):
        return f'http://127.0.0.1:{self.server_port}/v1'

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.release_requests.set()
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.server.lock:
            self.server.connections += 1
            self.server.open_connections += 1
        time.sleep(self.server.connection_delay)

    def finish(self):
        try:
            super().finish()
        finally:
            with self.server.lock:
                self.server.open_connections -= 1
                self.server.lock.notify_all()

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = body['messages'][-1]['content']
        with self.server.lock:
            self.server.requests.append((self.client_address[1], body['model'], text))
            self.server.active_requests += 1
            self.server.peak_requests = max(self.server.peak_requests, self.server.active_requests)
            self.server.lock.notify_all()
        try:
            if text == 'hold':
                self.server.release_requests.wait(5)
            if text == 'delay':
                time.sleep(.15)
            if text in ('401', '503'):
                self.send_json(int(text), {'error': {'message': 'local test error', 'type': 'test_error'}})
            elif body.get('stream'):
                self.send_stream(body, text)
            else:
                if text == 'slow':
                    time.sleep(.35)
                self.send_json(200, {'id': 'test', 'object': 'chat.completion', 'created': 1,
                    'model': body['model'], 'choices': [{'index': 0, 'finish_reason': 'stop',
                    'message': {'role': 'assistant', 'content': body['model']}}],
                    'usage': {'prompt_tokens': 10, 'completion_tokens': 2, 'total_tokens': 12}})
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        finally:
            with self.server.lock:
                self.server.active_requests -= 1
                self.server.lock.notify_all()

    def send_json(self, status, data):
        payload = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        self.wfile.flush()

    def send_stream(self, body, text):
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()

        def emit(data):
            payload = ('data: ' + (data if isinstance(data, str) else json.dumps(data)) + '\n\n').encode()
            self.wfile.write(f'{len(payload):x}\r\n'.encode() + payload + b'\r\n')
            self.wfile.flush()

        emit({'id': 'test', 'object': 'chat.completion.chunk', 'created': 1, 'model': body['model'],
              'choices': [{'index': 0, 'delta': {'content': body['model']}, 'finish_reason': None}]})
        if text == 'slow':
            time.sleep(.35)
        emit({'id': 'test', 'object': 'chat.completion.chunk', 'created': 1, 'model': body['model'],
              'choices': [{'index': 0, 'delta': {}, 'finish_reason': 'stop'}]})
        emit('[DONE]')
        self.wfile.write(b'0\r\n\r\n')
        self.wfile.flush()
