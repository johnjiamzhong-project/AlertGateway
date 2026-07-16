# AlertGateway Agent Instructions

Before performing any task in this repository:

1. Read `PROJECT_MEMORY.md` completely.
2. Read and follow `.agents/AGENTS.md`.
3. Inspect the current Git status and only the files relevant to the task.
4. Continue incrementally from the recorded project state and decisions. Do not redesign the whole project unless the user explicitly requests it.
5. Update `PROJECT_MEMORY.md` when verified project state, major decisions, completed work, or the next recommended action changes.

If memory, documentation, source code, Git state, and verified runtime results disagree, use the latest verifiable facts and correct `PROJECT_MEMORY.md` during the task.

## Fixed RTMP output address

For this project, every new or active inference-result push configuration must use the fixed SRS output address:

```text
rtmp://192.168.0.168/live/alertgateway
```

Do not create date-suffixed or mode-specific output stream names. Input/source publishing addresses used by test tools may remain separate when needed to feed SRS.
