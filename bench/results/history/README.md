# History

`2026-11-01-pilot-before-rescan-fix.jsonl`: one-repetition pilot run made before the full-page
rescan fix (commit "fix: bound full-page rescans by unreclaimed pages"). It is kept as evidence
of the memory blow-up that fix addresses: `prodcon 4pairs` peaked at 2417.8 MB over the baseline
RSS for heapwise against 0.4-24 MB for the other allocators. Its timings are not used anywhere:
the machine was not verified to be idle during this run.
