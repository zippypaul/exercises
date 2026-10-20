# Slow Mail - Test Log

Append one dated section per run. Do not edit old entries; add a correction entry instead. A result is only valid if it records the commit, environment and exact commands. Unrun items stay as "not run", never blank-as-pass.

Levels: simulator (desktop Qt UI on the host), fake server (contract tests against `tools/fake_graph/server.py`), real mailbox, device. See [TEST_PLAN.md](TEST_PLAN.md).

## Template (copy this block for each run)

### YYYY-MM-DD - <short title>

| Field | Value |
|---|---|
| Date / time (UTC) | |
| Tester / agent | |
| Commit id (`git rev-parse HEAD`) and dirty? | |
| Level | simulator / fake server / real mailbox / device |
| Host OS, compiler, CMake, Qt version | |
| Device model / OS build / SDK / launcher versions (device runs only) | |
| Mailbox | none (fake) / synthetic tenant / real (should be NOT YET) |

Commands run:

```
<exact commands>
```

Results:

| Item | Result (pass / fail / not run) | Evidence (log excerpt, file, photo) |
|---|---|---|
| core_tests | | |
| storage_tests | | |
| graph_tests | | |
| contract_tests | | |
| A01 ... A20 (list each one run) | | |
| Simulator smoke test | | |
| Device checklist items | | |

Deviations from the plan, known failures, follow-ups:

Assumptions updated (ASM-nn changed status):

## 2026-10-20 - First host build and test run (Ubuntu 24.04, simulator + fake server)

<integrator fills in>

Notes for the integrator:
- Record the real GoogleTest names from `--gtest_list_tests` and reconcile them with the "Test name pattern" column in TEST_PLAN.md.
- State clearly which acceptance items (A01-A20) have an automated test that passed, and which have none.
- Nothing in this entry may claim real-mailbox or device results. Those levels are NOT YET.
