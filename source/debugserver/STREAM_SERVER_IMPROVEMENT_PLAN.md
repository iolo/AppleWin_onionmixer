# TelnetStreamServer (65505) 개선 계획

## 현재 문제점

### 증상
- 클라이언트가 65505 포트에 연결하면 초기 스냅샷만 수신
- 이후 지속적인 데이터 스트리밍 없음

### 원인 분석
```
Debug.cpp:9095-9125
```
- `BroadcastStreamData()` 호출이 `if (bDoSingleStep)` 블록 안에만 존재
- 싱글 스텝 모드에서만 데이터 스트리밍
- 정상 실행(Running) 모드에서는 브로드캐스트 없음

---

## 개선 방안 비교

| 방안 | 장점 | 단점 | 성능 영향 |
|------|------|------|----------|
| 1. CPU 루프 직접 통합 | 실시간 | 매 명령마다 호출, 과도한 데이터 | 높음 |
| 2. 프레임 기반 (VSync) | 60Hz 주기, 안정적 | 비디오 코드 수정 필요 | 낮음 |
| 3. **별도 타이머 스레드** | 에뮬레이션과 분리, 유연한 주기 | 스레드 동기화 | 매우 낮음 |
| 4. 이벤트 기반 | 효율적, 변경 시에만 | 구현 복잡 | 낮음 |

### 권장: 방안 3 - 별도 타이머 스레드

**이유:**
1. 메인 에뮬레이션 루프 성능에 영향 없음
2. 클라이언트 연결 유무에 따라 동적 활성화/비활성화
3. 브로드캐스트 주기를 설정 가능하게 구현 가능
4. 기존 TelnetStreamServer 구조에 자연스럽게 통합

---

## 상세 구현 계획

### Phase 1: TelnetStreamServer에 주기적 브로드캐스트 스레드 추가

#### 1.1 TelnetStreamServer.h 수정

```cpp
// 추가할 멤버 변수
private:
    // Periodic broadcast
    std::thread m_broadcastThread;
    std::atomic<bool> m_broadcastRunning;
    std::atomic<int> m_broadcastIntervalMs;  // 기본값: 100ms

    // Broadcast loop
    void BroadcastLoop();

public:
    // 브로드캐스트 간격 설정 (밀리초)
    void SetBroadcastInterval(int intervalMs);
    int GetBroadcastInterval() const;

    // 브로드캐스트 활성화/비활성화
    void SetPeriodicBroadcastEnabled(bool enabled);
    bool IsPeriodicBroadcastEnabled() const;
```

#### 1.2 TelnetStreamServer.cpp 수정

```cpp
// Start() 함수에 추가
bool TelnetStreamServer::Start() {
    // ... 기존 코드 ...

    // Start periodic broadcast thread
    m_broadcastRunning.store(true);
    m_broadcastThread = std::thread([this]() {
        BroadcastLoop();
    });

    return true;
}

// Stop() 함수에 추가
void TelnetStreamServer::Stop() {
    // Stop broadcast thread
    m_broadcastRunning.store(false);
    if (m_broadcastThread.joinable()) {
        m_broadcastThread.join();
    }

    // ... 기존 코드 ...
}

// 새로운 BroadcastLoop 함수
void TelnetStreamServer::BroadcastLoop() {
    while (m_broadcastRunning.load()) {
        // 연결된 클라이언트가 있을 때만 브로드캐스트
        if (GetClientCount() > 0 && m_provider) {
            // 현재 상태 스냅샷 가져오기
            auto snapshot = m_provider->GetPeriodicUpdate();
            for (const auto& line : snapshot) {
                Broadcast(line);
            }
        }

        // 설정된 간격만큼 대기
        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_broadcastIntervalMs.load())
        );
    }
}
```

### Phase 2: DebugStreamProvider에 주기적 업데이트 메서드 추가

#### 2.1 DebugStreamProvider.h 수정

```cpp
public:
    // 주기적 업데이트용 (간소화된 데이터)
    std::vector<std::string> GetPeriodicUpdate();

    // 업데이트 레벨 설정
    enum class UpdateLevel {
        Minimal,    // PC, A, X, Y, SP, P만
        Standard,   // + 플래그, 메모리 모드
        Full        // + Zero Page, Stack, Text Screen
    };

    void SetUpdateLevel(UpdateLevel level);
    UpdateLevel GetUpdateLevel() const;

private:
    UpdateLevel m_updateLevel = UpdateLevel::Standard;
```

#### 2.2 DebugStreamProvider.cpp 수정

```cpp
std::vector<std::string> DebugStreamProvider::GetPeriodicUpdate() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> lines;

    // 타임스탬프 추가
    std::map<std::string, std::string> tsExtra;
    tsExtra["ts"] = std::to_string(GetTimestamp());

    switch (m_updateLevel) {
        case UpdateLevel::Minimal:
            // CPU 레지스터만
            lines.push_back(FormatLine("cpu", "reg", "pc", ToHex16(regs.pc), tsExtra));
            lines.push_back(FormatLine("cpu", "reg", "a", ToHex8(regs.a)));
            lines.push_back(FormatLine("cpu", "reg", "x", ToHex8(regs.x)));
            lines.push_back(FormatLine("cpu", "reg", "y", ToHex8(regs.y)));
            lines.push_back(FormatLine("cpu", "reg", "sp", ToHex8(regs.sp & 0xFF)));
            lines.push_back(FormatLine("cpu", "reg", "p", ToHex8(regs.ps)));
            break;

        case UpdateLevel::Standard:
            // Minimal + 플래그 + 메모리 모드
            // ... Minimal 내용 ...
            // + CPU flags
            // + Memory mode flags
            break;

        case UpdateLevel::Full:
            // Standard + Zero Page + Stack + Text
            // ... Standard 내용 ...
            // + Zero Page dump
            // + Stack page dump
            // + Text screen
            break;
    }

    return lines;
}
```

### Phase 3: 머신 상태 변경 이벤트 통합

에뮬레이터 상태 변경 시 즉시 브로드캐스트:

#### 3.1 상태 변경 이벤트 목록

| 이벤트 | 브로드캐스트 내용 |
|--------|------------------|
| 머신 시작/정지 | `mach.status.mode` |
| 브레이크포인트 히트 | `dbg.bp.hit` |
| 리셋 | `sys.event.reset` |
| 디스크 삽입/제거 | `sys.event.disk` |

#### 3.2 Core.cpp 또는 해당 파일에 훅 추가

```cpp
// 예: 머신 상태 변경 시
void SetAppMode(AppMode_e newMode) {
    AppMode_e oldMode = g_nAppMode;
    g_nAppMode = newMode;

    // 디버그 스트림에 상태 변경 알림
    if (DebugServer_IsStreamEnabled() && oldMode != newMode) {
        auto& manager = debugserver::DebugServerManager::GetInstance();
        auto* provider = manager.GetStreamProvider();
        if (provider) {
            const char* modeStr = "unknown";
            switch (newMode) {
                case MODE_RUNNING: modeStr = "running"; break;
                case MODE_PAUSED:  modeStr = "paused"; break;
                case MODE_DEBUG:   modeStr = "debug"; break;
                // ...
            }
            manager.BroadcastStreamData(provider->GetMachineStatus(modeStr));
        }
    }
}
```

---

## 구현 우선순위

### 1단계 (필수)
- [ ] TelnetStreamServer에 BroadcastLoop 스레드 추가
- [ ] DebugStreamProvider에 GetPeriodicUpdate() 추가
- [ ] 기본 브로드캐스트 간격 100ms 설정

### 2단계 (권장)
- [ ] UpdateLevel 설정 기능 추가
- [ ] 클라이언트 명령으로 간격/레벨 조절 가능하게

### 3단계 (선택)
- [ ] 이벤트 기반 브로드캐스트 (상태 변경 시)
- [ ] 클라이언트별 구독 설정

---

## 수정 파일 목록

| 파일 | 수정 내용 |
|------|----------|
| `TelnetStreamServer.h` | BroadcastLoop 스레드 관련 멤버/메서드 추가 |
| `TelnetStreamServer.cpp` | BroadcastLoop 구현, Start/Stop 수정 |
| `DebugStreamProvider.h` | GetPeriodicUpdate, UpdateLevel 추가 |
| `DebugStreamProvider.cpp` | GetPeriodicUpdate 구현 |
| `DebugServerManager.cpp` | (선택) 설정 인터페이스 추가 |

---

## 테스트 계획

### 기능 테스트
1. 클라이언트 연결 시 초기 스냅샷 수신 확인
2. 연결 후 주기적 데이터 수신 확인 (100ms 간격)
3. 에뮬레이터 실행/정지 상태에서 모두 스트리밍 확인
4. 다중 클라이언트 연결 테스트

### 성능 테스트
1. 스트리밍 활성화 시 에뮬레이션 속도 영향 측정
2. 메모리 사용량 모니터링
3. CPU 사용률 확인

### 테스트 명령
```bash
# 연결 테스트
nc 127.0.0.1 65505

# 또는 telnet
telnet 127.0.0.1 65505
```

---

## 예상 결과

수정 후 65505 포트 연결 시:
```json
{"emu":"apple","cat":"sys","sec":"conn","fld":"hello","val":"AppleWin Debug Stream","ver":"1.0","ts":1704067200000}
{"emu":"apple","cat":"mach","sec":"info","fld":"type","val":"Apple2eEnhanced"}
... (초기 스냅샷) ...
{"emu":"apple","cat":"cpu","sec":"reg","fld":"pc","val":"C600","ts":1704067200100}
{"emu":"apple","cat":"cpu","sec":"reg","fld":"a","val":"00"}
... (100ms 후) ...
{"emu":"apple","cat":"cpu","sec":"reg","fld":"pc","val":"C603","ts":1704067200200}
... (지속적인 스트리밍) ...
```
