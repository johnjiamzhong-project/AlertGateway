# AlertGateway Agent Instructions

Before performing any task in this repository:

1. Read `PROJECT_MEMORY.md` completely.
2. Read and follow `.agents/AGENTS.md`.
3. Inspect the current Git status and only the files relevant to the task.
4. Continue incrementally from the recorded project state and decisions. Do not redesign the whole project unless the user explicitly requests it.
5. Update `PROJECT_MEMORY.md` when verified project state, major decisions, completed work, or the next recommended action changes.
6. If memory, documentation, source code, Git state, and verified runtime results disagree, use the latest verifiable facts and correct `PROJECT_MEMORY.md` during the task.

## Fixed multi-channel RTMP output addresses

For this project, the legacy single-channel configuration keeps its result address
`rtmp://192.168.0.168/live/alertgateway`. Every channel in a multi-channel `channels[]`
configuration must publish to an independent SRS RTMP output address using the following fixed template:

```text
rtmp://192.168.0.168/live/alertgateway_channel_{channel_id}
```
