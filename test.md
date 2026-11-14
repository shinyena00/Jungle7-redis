# 조각 1 테스트 기록 — store.c

- 날짜: 2026-11-12
- 브랜치: `yena`
- 대상: `src/store.c` (FNV-1a, 체이닝, 칸 고정, 게으른 만료)
- 환경: WSL Ubuntu, `cc -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE`

## 1. 단위 테스트 (정상 코드)

```bash
cd tests && make unit
```

> Makefile은 레포 루트가 아니라 `tests/` 안에 있다. 루트에서 `make unit`을 하면 `No rule to make target 'unit'` 에러가 난다.

| 테스트 | 확인 내용 | 결과 |
|---|---|---|
| u01_set_then_get | SET 후 GET | ok |
| u02_overwrite | 같은 키 덮어쓰기 | ok |
| u03_missing | 없는 키 | ok |
| u04_collision_9_keys | 8칸에 9개 → 충돌 체인 | ok |
| u05_07_delete_each_chain_position | 체인 앞/중간/끝 삭제 | ok |
| u08_nul_in_key | 키 안의 `\0` | ok |
| u09_del_then_get | 삭제 후 GET | ok |
| u10_del_then_set | 삭제 후 다시 SET | ok |
| u11_before_expire | 만료 전 | ok |
| u12_at_expire_still_visible | 정각엔 아직 보임 | ok |
| u13_after_expire_gone | 지나면 사라짐 | ok |
| u14_set_over_expired_node | 만료 노드 위에 SET | ok |
| u15_no_expire | 영구(-1) | ok |
| u16_expire_changes_only_deadline | EXPIRE는 기한만 바꿈 | ok |
| u17_reset_restarts_deadline | 재SET이면 기한 새로 시작 | ok |
| u18_expired_key_del_and_expire | 만료 키에 DEL/EXPIRE → 0 | ok |

**결과: 16 tests, 0 failed** (경고 0)

## 2. 일부러 깨보기

원본은 그대로 두고, 임시 복사본만 고쳐서 `STORE_SRC`로 빌드했다.
테스트가 실제로 규칙을 지키고 있는지 확인하려는 단계다.

### ① 만료 경계 `>` → `>=`

```diff
-        && now_ms > entry->expire_at_ms;
+        && now_ms >= entry->expire_at_ms;
```

| 깨진 테스트 | 위치 |
|---|---|
| u12_at_expire_still_visible | test_store.c:165 `get_is(s, S("k"), EXP, S("v"))` |
| u16_expire_changes_only_deadline | test_store.c:205 `get_is(s, S("k"), new_exp, S("v"))` |

**결과: 16 tests, 2 failed**

- 규칙은 `now > expire_at`일 때 만료다. 그래서 정각(`now == expire_at`)에는 아직 보여야 한다.
- 예상한 U12에 더해 U16도 잡혔다. U16은 EXPIRE로 바꾼 새 기한의 정각을 확인한다.

### ② 키 비교 `memcmp` → `strcmp`

```diff
-    return entry->klen == klen && memcmp(entry->key, key, klen) == 0;
+    return entry->klen == klen && strcmp(entry->key, key) == 0;
```

| 깨진 테스트 |
|---|
| u04, u05_07, **u08**, u09, u10, u11, u12, u14, u15, u16, u17 |

**결과: 16 tests, 11 failed** (u01~u03, u13, u18은 통과)

strcmp가 틀린 이유는 두 가지다.

1. **`\0`이 든 키 (u08)**: `strcmp`는 `\0`에서 비교를 멈춘다. 그래서 `"a\0b"`와 `"a\0c"`를 같은 키로 본다.
2. **키 버퍼에 `\0` 끝이 없다 (나머지)**: `copy_bytes`는 길이만큼만 malloc하고 `\0`을 붙이지 않는다. `strcmp`는 버퍼 끝을 넘어 쓰레기 메모리까지 읽는다(정의되지 않은 동작). 그래서 같은 키를 다르다고 판단한다. 통과한 테스트는 운이 좋았을 뿐이다.

→ 키 비교는 **길이 먼저, 그다음 memcmp**가 맞다.

## 3. 정리

| 확인 | 결과 |
|---|---|
| 정상 단위 테스트 | 16/16 통과 |
| `>=` 깨보기 | U12, U16에서 잡힘 |
| `strcmp` 깨보기 | U8 포함 11개에서 잡힘 |
| 원본 `src/store.c` | 변경 없음 |

다음 할 일: resp.c → cmd.c → server.c(epoll) → `make e2e` (E1~E32)

---

# 조각 1 테스트 기록 — command.c · resp.c · 만료 키 정리

- 날짜: 2026-11-13
- 브랜치: `yena` (PR #3에 #4 도현, #5 지현 코드를 합침)
- 대상: `src/command.c`, `src/resp.c`, `src/store.c`(`store_purge_expired` 추가)
- 환경: Windows MSYS2 UCRT64 gcc, `-std=c11 -Wall -Wextra -D_GNU_SOURCE`

## 1. 이번에 정한 것

| 내용 | 결정 | 가져온 곳 |
|---|---|---|
| 판단·번역 나누기 | command.c는 응답 종류(`reply_t`)만, RESP 바이트는 resp.c | #3 |
| 기간 숫자 | 한 번만 읽기. 곱하기 넘침 → 더하기 넘침 두 단계로 검사 | #4 |
| `EX 01` | 잘못된 값 (첫 글자 1~9만) | #3·#5 |
| 주석 | 파일 맨 위 흐름 트리 + 경우별 "입력 → 결과" 표 | #5 |
| 메모리 부족 | 만료 키 전부 정리 후 한 번 재시도, 그래도 실패면 `메모리가 부족합니다` + 연결 유지 | 새로 정함 |
| 단어 0개(`*0`) | resp.c가 형식 에러 | 새로 정함 |
| 입력 크기 한도 | 모든 단어 1MB, 두 번째 단어(키) 256B | 노션 11/13 |
| 입력 개수 한도 | 단어 6개 이상이면 `*N`만 보고 형식 에러 + 끊김 (`RESP_MAX_ARGS` 16 → 5) | 노션 11/13 |

> 만료 방식: 평소엔 게으른 만료(건드릴 때 지움), 메모리가 부족할 때만 전체를 훑어 정리한다.
> `store_purge_expired`는 store.h에 새로 추가한 함수라 팀 확인이 필요하다.

## 2. 단위 테스트

```bash
gcc -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE -Iinclude \
    -o test_store tests/unit/test_store.c src/store.c && ./test_store
```

**결과: 16 tests, 0 failed** (경고 0). `store_purge_expired`를 추가한 뒤에도 그대로 통과.

## 3. 외부 테스트 수정 (E29, E33~E36)

노션 11/13 입력 한도 결정에 맞춰 `tests/e2e/test_resp.c`를 고쳤다. **⚠️ AI 초안, 사람 검토 필요.**

| 테스트 | 내용 |
|---|---|
| E29 (수정) | `SET k v EX 10 EX 20`(7개) → 모르는 명령이 아니라 **형식 에러 + 끊김**. 원래 값 GET은 새 연결에서 |
| E33 | 명령 이름 자리 `$2000000` → 너무 큽니다 + 끊김 |
| E34 | 4번째·5번째 단어 `$1048577` → 너무 큽니다 + 끊김 |
| E35 | 모르는 명령 + 두 번째 단어 257B → 너무 큽니다 + 끊김 (검사 2가 3보다 먼저) |
| E36 | `*6` → 형식 에러 + 끊김, `*5`(SET k v EX n)는 정상 |

한도를 넘는 쪽은 E16처럼 길이 머리까지만 보낸다. 본문까지 보내면 서버가 안 읽고 끊을 때 RST로 에러 응답을 못 받을 수 있다.

**`make e2e`는 아직 못 돌림**: `server.c`가 없고, `resp_client.c`가 `arpa/inet.h`(POSIX 소켓)를 써서 Windows gcc로는 컴파일이 안 된다. `test_resp.c`는 문법 검사(`-fsyntax-only`)만 통과.

## 4. 서버 없이 이어 붙여 확인 (임시 프로그램, 레포에 안 넣음)

`resp_parse → command_execute → resp_write_reply`를 그대로 이어서 요청 바이트 → 응답 바이트를 비교했다.
메모리 부족은 store.c의 malloc을 일부러 실패하게 바꿔(`-include`로 malloc 바꿔치기) 확인했다.

| 묶음 | 확인 내용 | 결과 |
|---|---|---|
| E29 | EX만 → 기간 필요, FOO → 모르는 명령, 7개 → 형식 에러 + 끊김, 원래 값 유지 | 5/5 |
| E33~E36 | 위 표 그대로 + `*5`·키 256B·값 1MB 정확히 → 아직(기다림), `*0` → 형식 에러 | 10/10 |
| 기간 숫자 | `01`, `-01`, `0`, `+1`, `" 1"`, `1abc`, 20자리 거절 / `-1` 영구 / int64 상한 정확히 OK·+1 거절 / E13·E18·E30 | 18/18 |
| 메모리 부족 | 만료 키 있으면 정리 후 OK(영구 키는 남음) / 만료 키 없으면 바로 `메모리가 부족합니다` + 원래 값 유지 / 정리해도 계속 실패면 `메모리가 부족합니다` | 12/12 |

**결과: 45 checks, 0 failed**

## 5. 정리

| 확인 | 결과 |
|---|---|
| 경고 (`-Wall -Wextra`) | 0 |
| 단위 테스트 | 16/16 통과 |
| 임시 이어 붙이기 확인 | 45/45 통과 |
| 외부 테스트 E1~E36 | 미실행 (server.c 없음) |

다음 할 일: server.c(epoll) → WSL에서 `make e2e` (E1~E36) → 테스트 사람 검토

---

# 조각 1 테스트 기록 — server.c 1·2단계

- 날짜: 2026-11-14
- 브랜치: `yena`
- 대상: `src/server.c` (epoll level-triggered, 스레드 하나), 루트 `Makefile`
- 환경: WSL2 Ubuntu (커널 6.6), `cc -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE`

## 1. 이번에 넣은 것

| 단계 | 내용 |
|---|---|
| 1 | `MINI_REDIS_BIND`(기본 127.0.0.1)·`MINI_REDIS_PORT`(기본 6380) 읽기, SIGPIPE 무시, 받는 소켓, epoll 루프, accept, 연결 정리 |
| 2 | recv 한 번 → `resp_parse` → `command_execute` → `resp_write_reply` → send, 못 다 보낸 것만 `EPOLLOUT`으로 이어 보내기 |
| 2 | 형식 에러·너무 큼: 에러를 붙이고 읽기를 멈춘 뒤(`EPOLLOUT`만 감시) 다 보내면 닫기 |

아직 없는 것: 한 바퀴 100개(3단계), 4MB 한도·버퍼 메모리 부족 시 purge(4단계, 지금은 바로 닫음), 유휴 타임아웃(5단계), EMFILE 처리(6단계).

## 2. 빌드

```bash
make server        # 레포 루트. ./mini-redis
```

경고 0.

## 3. 1단계 확인

| 확인 | 결과 |
|---|---|
| 잘못된 포트 `abc` `0` `70000` `80x` `""` `" 80"` `+80` 20자리 | 이유 출력 후 exit 1 |
| 잘못된 주소 `localhost` `1.2.3` `""` | exit 1 |
| 같은 포트로 하나 더 | `bind 실패: Address already in use`, exit 1 |
| 접속 10개 → 전부 끊기, 50번 붙었다 끊기, 5MB 보내기 | 열린 fd 수가 처음과 같음, 서버 살아 있음 |
| CPU (접속 유지 중·조용할 때) | 0~0.5% |

## 4. 외부 테스트 (처음으로 실제 서버)

```bash
./mini-redis &
cd tests && make e2e
```

**결과: 36 tests, 0 failed** (E1~E36). AddressSanitizer 빌드(`-fsanitize=address,undefined`)로 다시 돌려도 36/36, ASan 경고 0.

> WSL에 valgrind가 없어서 ASan으로 대신했다. 서버를 시그널로 끄기 때문에 종료 시 누수 검사는 안 됨.

## 5. 추가 확인 (임시 스크립트, 레포에 안 넣음, ASan 빌드)

| 확인 | 결과 |
|---|---|
| 1MB 값(`\0`·`\r\n` 포함) SET → GET | 같은 바이트 |
| `SET` + `GET` + 깨진 RESP를 한 번에 | `+OK`, `$1 v`, `-ERR 잘못된 요청 형식입니다` 받은 뒤 끊김 |
| 1MB GET 20개 + 깨진 RESP, 2초 동안 안 읽음 | 그동안 CPU 거의 0, 연결 유지 (닫는 중) |
| 이어서 다 읽기 | 20MB + 에러 전부 도착 후 끊김 |
| 1MB GET 30개 보내고 안 읽고 RST로 끊기 × 5 (E37 미리 보기) | 서버 살아 있음 |
| 그 뒤 새 연결 | 정상 |
| 열린 fd 수 | 처음과 같음 |

**결과: 11 checks, 0 failed**

## 6. 정리

| 확인 | 결과 |
|---|---|
| 경고 | 0 |
| 외부 테스트 E1~E36 | 36/36 (일반·ASan) |
| 추가 확인 | 11/11 |

다음 할 일: 3단계(한 바퀴 100개 + 결정 다) → 4단계(4MB 한도, purge) → 5단계(유휴 타임아웃) → 6단계(EMFILE)

---

# 조각 1 테스트 기록 — server.c 3단계 (공정성)

- 날짜: 2026-11-14
- 브랜치: `yena`
- 대상: `src/server.c`
- 환경: WSL2 Ubuntu, `cc -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE`

## 1. 이번에 넣은 것

| 내용 | 방법 |
|---|---|
| 한 바퀴 100개 | `run_commands`가 연결 하나에서 최대 `COMMANDS_PER_ROUND`(100)개만 처리. 빌드할 때 `-DCOMMANDS_PER_ROUND=2`로 바꿀 수 있음 |
| 결정 (다) | 완성된 명령이 남아 있으면 recv하지 않음 (`receive_and_run` → `has_complete_command`) |
| 대기 목록 | 100개를 채우고도 남은 연결을 넣는 이중 연결 목록. 들어간 바퀴(`pending_round`)를 적어 두고, 지난 바퀴 것만 처리해서 한 바퀴에 100개를 넘지 않게 함 |
| 대기 중 감시 조건 | 대기 중엔 `EPOLLIN`을 끔 (어차피 recv하지 않으니까) |
| 대기 시간 | 대기 목록이 비어 있지 않으면 `epoll_wait` 대기 0, 비면 다시 1000ms |
| 닫기 | `close_connection`이 대기 목록에서도 뺌 |

## 2. 외부 테스트

| 빌드 | 결과 |
|---|---|
| 일반 (100개) | 36/36 |
| `-DCOMMANDS_PER_ROUND=2` | 36/36 |
| AddressSanitizer (100개) | 36/36, ASan 경고 0 |

경고 0.

## 3. 추가 확인 (임시 스크립트, 레포에 안 넣음, 세 빌드 모두)

| 확인 | 결과 |
|---|---|
| SET 3000개 + GET 3000개를 한 연결로 붙여 보내기 | 순서·값 그대로 |
| A가 GET 30만 개를 붙여 보내는 중에 B가 GET 20번 | B 최악 0.4ms(일반) / 2.0ms(한도 2) / 1.0ms(ASan), B가 A보다 먼저 끝남 |
| 34MB를 붙여 보내며 응답은 계속 읽기 | 응답 전부 받음, 서버 최대 메모리 증가 8MB 미만 |
| 다 끝난 뒤 2초 | CPU 거의 0 (대기 0ms로 계속 돌지 않음) |
| 처리 중인 연결을 RST로 끊기 × 10 | 이후 새 연결 정상 |

**결과: 8 checks × 3 빌드, 0 failed**

## 4. 아직 확인 못 한 것

- **일부러 깨보기를 안 했다.** (다)를 빼거나 100개 제한을 없앤 빌드로 위 확인을 돌려 보려다 중단했다.
  2단계 코드도 recv 한 번이 16KB라 한 이벤트에 처리하는 명령 수가 저절로 수백 개로 묶인다.
  그래서 위 공정성·메모리 확인은 3단계 규칙이 없어도 통과할 수 있고, 규칙이 "효과가 있다"는 증거로는 약하다.
- 사람이 추가할 테스트 E38(한 바퀴 100개)이 이 빈칸을 채워야 한다.

다음 할 일: 4단계(출력 4MB 한도, 버퍼 메모리 부족 시 purge)

---

# 조각 1 테스트 기록 — server.c 4·5·6단계

- 날짜: 2026-11-14
- 브랜치: `yena`
- 대상: `src/server.c`
- 환경: WSL2 Ubuntu, `cc -std=c11 -Wall -Wextra -g -O0 -D_GNU_SOURCE`

## 1. 이번에 넣은 것

| 단계 | 내용 |
|---|---|
| 4 | 안 보낸 응답이 4MB(`OUTPUT_LIMIT`)를 넘으면 **send를 한 번 해 보고**, 그래도 넘으면 `-ERR 응답이 너무 많이 쌓였습니다`를 붙여 send 한 번 시도 후 닫기 |
| 4 | 입력·출력 버퍼를 늘리다 실패하면 `store_purge_expired` 후 한 번 다시 시도, 그래도 안 되면 닫기. 출력은 `command_execute`에 넘긴 `now_ms` 그대로(🔒 조건 1), 입력은 그 순간 시각. 지운 게 없으면 다시 시도하지 않음 |
| 5 | 유휴 목록(마지막 활동 순서 이중 연결 목록). 활동 = accept / recv 1바이트 이상 / send 1바이트 이상. 1초마다 맨 앞부터 보고 120초(`IDLE_TIMEOUT_MS`) 지난 연결 닫기 |
| 6 | `accept4`가 `EMFILE`·`ENFILE`·`ENOBUFS`·`ENOMEM`이면 받는 소켓을 epoll에서 빼고, 연결이 하나 닫히면 다시 넣기 |

> **"send를 한 번 해 보고"를 덧붙인 이유**: 명령 100개를 다 처리한 뒤에야 send하므로, 응답을 잘 읽는 클라이언트도 1MB GET 5개를 붙여 보내면 잠깐 4MB를 넘는다. 결정 이유가 "안 읽어서 쌓인 것"이라 이 경우는 끊지 않게 했다.

## 2. 외부 테스트

| 빌드 | 결과 |
|---|---|
| 일반 | 36/36 |
| `-DCOMMANDS_PER_ROUND=2` | 36/36 |
| AddressSanitizer | 36/36, ASan 경고 0 |

경고 0. (80자 넘는 줄은 사람이 단 `//` 주석 한 줄뿐)

## 3. 추가 확인 (임시 스크립트, 레포에 안 넣음)

### 출력 4MB 한도 (일반·한도 2·ASan 세 빌드)

| 확인 | 결과 |
|---|---|
| 안 읽고 1MB GET 50개 | 서버가 닫음, 클라이언트도 EOF/RST |
| 읽으면서 1MB GET 8개 붙여 보내기 | 8MB 전부 받고 연결 유지 |
| 그 뒤 새 연결, fd 수 | 정상, 원래대로 |

### 버퍼 메모리 부족 (`-Wl,--wrap=realloc`으로 1MB 이상 realloc을 일부러 실패)

| 확인 | 결과 |
|---|---|
| 출력 realloc 1번 실패 + 만료 키 있음 | purge(지움=1) → 다시 시도 → GET 1MB 정상 |
| 출력 realloc 실패 + 만료 키 없음 | purge(지움=0) → 닫힘 |
| 입력 realloc 1번 실패 + 만료 키 있음 | purge(지움=1) → 1MB SET +OK |
| 입력 realloc 계속 실패 | 닫힘, 그 뒤 새 연결과 기존 값 정상 |

### 유휴 타임아웃 (`-DIDLE_TIMEOUT_MS=3000` 빌드)

| 확인 | 결과 |
|---|---|
| 1초마다 GET 하는 연결 | 6.5초 동안 계속 응답, 열려 있음 |
| 조용한 연결 | 닫힘 |
| 3MB 응답을 안 읽어 막힌 연결 (EAGAIN은 활동 아님) | 닫힘 |
| 접속만 하고 가만히 | 3~4.5초 사이에 닫힘 |

### fd 꽉 참 (`ulimit -n 32`)

| 확인 | 결과 |
|---|---|
| 한도보다 10개 많이 접속 | 서버 fd가 한도까지 차고, 2초 동안 CPU 거의 0 |
| 멈춤 로그 | 2번 (쏟아지지 않음) |
| 자리 없는 10개 | backlog에서 기다림 (응답 없음) |
| 받아진 연결 10개를 닫음 | 기다리던 10개가 받아져서 응답 |

**결과: 출력 한도 6 × 3 빌드, 메모리 부족 8, 유휴 7, fd 7 — 0 failed**

## 4. 정리

| 확인 | 결과 |
|---|---|
| 경고 | 0 |
| 외부 테스트 E1~E36 | 36/36 (일반·한도 2·ASan) |
| 4·5·6단계 추가 확인 | 40/40 |

남은 것

- `-ERR 응답이 너무 많이 쌓였습니다` 문구가 클라이언트에 닿는지는 확인하지 않았다 (결정대로 거의 안 닿음).
- 3단계 일부러 깨보기, 사람이 추가할 E37·E38은 그대로 남아 있다.
- 팀 확인: store.h 주석(purge가 남는 값의 주소를 안 바꿈), 노션 🔒 예외(입력·출력 버퍼 메모리 부족 때만 purge), 새 에러 문구.
