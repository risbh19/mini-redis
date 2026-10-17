# Validation record

Date: 18 October 2026 (Asia/Kolkata). Local validation uses Ubuntu under WSL2.

| Check | Result recorded so far |
|---|---|
| GCC 15.2 Debug | Unit and black-box TCP integration suite passed |
| Clang 21.1.8 Release | 48 CTest entries passed (47 unit/property tests + one integration entry) |
| ASan + UBSan | Prior 47-entry suite passed; final rebuild queued |
| TSan | Prior 47-entry suite passed; final rebuild queued |
| RESP parser fuzz | Ten-minute local run in progress |
| AOF decoder fuzz | Ten-minute local run in progress |
| Coverage | Instrumented build in progress; no percentage claimed yet |
| Docker / GitHub Actions | Workflow provided; remote execution pending |

The integration entry contains twelve black-box test methods, with subtests for
both I/O models where applicable. It covers binary data, partial requests,
pipelining, half-close, concurrent counters, idle-client worker starvation,
1000 idle reactor connections, slow readers, disconnects, limits, shutdown,
SIGKILL recovery, truncated AOF transactions, and corrupt-file startup refusal.

Final raw results and benchmark context will be recorded after runs complete.
