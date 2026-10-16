# Historical logging-OFF control — WSL2 / g++ 14

These four `savina-bank-transaction/qb__{1c,2c}-{spin,park}.json` documents are the
shipped-3.1.0 control rebuilt with `QB_WITH_LOGGING=OFF`, described in `docs/TUNING.md`
§12.2–12.3. They are distinct from the logging-ON shipped control and the candidate grids.

Their `caveats` arrays incorrectly say logging was left ON: the adapter emitted that text
unconditionally. Read the build profile above for this cohort (QB-786). The four raw
JSON documents, samples, units and summaries are unchanged. Each repetition measures
50,000 transfers; the p50 values remain the historical observations, not a new run.

This note corrects provenance only. It does not certify a current build's logging setting
or supply the separate ON/OFF producer controls required by QB-786.
