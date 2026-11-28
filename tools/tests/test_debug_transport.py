"""Wire contracts shared by title-owned debug scenarios."""
import json
from pathlib import Path
import socket
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from debug_transport import DebugClient, DebugError


class TransportTest(unittest.TestCase):
    def exchange(self, reply, **kwargs):
        server = socket.socket()
        server.bind(('127.0.0.1', 0))
        server.listen()
        port = server.getsockname()[1]
        def serve():
            with server:
                with server.accept()[0] as connection:
                    request = bytearray()
                    while b'\n' not in request:
                        request.extend(connection.recv(1024))
                    response = reply(json.loads(request))
                    for chunk in response:
                        connection.sendall(chunk)
        thread = threading.Thread(target=serve)
        thread.start()
        try:
            return DebugClient(port, **kwargs).request('fixture', text='braces { and quotes "')
        finally:
            thread.join()

    def test_fragmented_response_and_request_id(self):
        response = self.exchange(lambda r: [b'{"id":', str(r['id']).encode(), b',"ok":true,"value":"}\\\"{"}\n'])
        self.assertEqual(response['value'], '}"{')

    def test_mismatched_id_rejected(self):
        with self.assertRaisesRegex(DebugError, 'mismatched response id'):
            self.exchange(lambda r: [b'{"id":12,"ok":true}\n'])

    def test_error_response_rejected(self):
        with self.assertRaisesRegex(DebugError, 'runtime rejected'):
            self.exchange(lambda r: [b'{"id":1,"ok":false,"error":"fixture"}\n'])

    def test_truncated_response_rejected(self):
        with self.assertRaisesRegex(DebugError, 'complete response'):
            self.exchange(lambda r: [b'{"id":1,"ok":true}'])

    def test_response_limit_and_extra_data(self):
        with self.assertRaisesRegex(DebugError, 'exceeds 64'):
            self.exchange(lambda r: [b'{"id":1,"ok":true,"x":"'+b'x'*64+b'"}\n'], max_response_bytes=64)
        with self.assertRaisesRegex(DebugError, 'extra response data'):
            self.exchange(lambda r: [b'{"id":1,"ok":true}\n{"ok":true}\n'])

    def test_invalid_request_never_connects(self):
        client = DebugClient(1234)
        with self.assertRaises(ValueError):client.request('fixture', id=1)
        with self.assertRaises(ValueError):client.request('fixture', value=float('nan'))


if __name__ == '__main__':
    unittest.main()
