import socket
import time
import json
import threading

class XV6Bridge:
    def __init__(self, host='127.0.0.1', port=4444):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    def connect(self):
        print(f"[*] xv6({self.host}:{self.port})에 연결을 시도합니다...")
        while True:
            try:
                self.sock.connect((self.host, self.port))
                print("[+] xv6 연결 성공!\n")
                break
            except ConnectionRefusedError:
                time.sleep(1)
                
        # 연결 성공 후, xv6의 출력을 계속 읽어오는 스레드 시작
        threading.Thread(target=self.receive_output, daemon=True).start()

    def receive_output(self):
        """xv6에서 나오는 텍스트를 파이썬 터미널에 출력합니다."""
        while True:
            try:
                data = self.sock.recv(1024)
                if data:
                    # xv6의 출력을 파이썬 화면에 표시 (초록색으로 표시)
                    print(f"\033[92m{data.decode('utf-8', errors='ignore')}\033[0m", end='', flush=True)
            except:
                break

    def send_command(self, cmd_string):
        payload = f"{cmd_string}\n"
        self.sock.sendall(payload.encode('utf-8'))
        print(f"\n[파이썬 -> xv6 전송] {cmd_string}")

    def close(self):
        self.sock.close()

if __name__ == "__main__":
    bridge = XV6Bridge()
    bridge.connect()

    # xv6 부팅이 완료될 때까지 2초 정도 여유를 줍니다.
    time.sleep(2) 

    # 가상의 LLM 응답 테스트
    mock_llm_response = '{"action": "kill", "pid": 4}'
    llm_data = json.loads(mock_llm_response)
    
    if llm_data['action'] == 'kill':
        target_pid = llm_data['pid']
        custom_cmd = f"KILL|{target_pid}"
        
        # xv6로 전송!
        bridge.send_command(custom_cmd)

    # xv6의 반응을 볼 수 있도록 파이썬을 종료하지 않고 10초 대기
    time.sleep(10)
    bridge.close()