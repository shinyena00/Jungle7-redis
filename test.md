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
