# Jungle7 미니 Redis

응답 캐시용 키-값 저장소. C, TCP 위 RESP, 스레드 하나 이벤트 루프.
요구사항·결정 사항은 노션 "레디스 구현 계획" 페이지를 따른다.

```
include/   팀이 합의한 인터페이스 (store.h: 해시 테이블 함수 모양)
src/       구현 (조각 1부터)
tests/     조각 1 테스트 (unit: 해시 테이블 직접 호출, e2e: 서버에 RESP로 요청)
```

테스트 실행 방법과 현재 상태는 [tests/README.md](tests/README.md).
