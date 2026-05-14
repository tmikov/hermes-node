## Fixing a Hermes NAPI Bug

When you discover a bug in Hermes's Node-API implementation, follow the
steps below. If you encounter multiple unrelated NAPI bugs, handle them
one at a time — complete the full cycle below for each bug sequentially
(they share the same repo, so they cannot be fixed in parallel).

1. Spin up a subagent (Task tool) to fix it in `/home/tmikov/work/hermes-n-api`.
   That repo has its own `CLAUDE.md` with build instructions and conventions.
2. The subagent must:
   - Create a failing test case under `test/napi/` and verify it fails.
   - Fix the bug **in the NAPI adapter layer** (`API/napi/`). Do NOT
     modify Hermes VM internals (`lib/VM/`, `lib/IRGen/`, etc.).
   - Verify the new test passes.
   - Verify all Hermes tests pass.
   - Commit the fix in hermes-n-api.
3. After the subagent completes, update the Hermes submodule in this repo
   to point to the new commit. The submodule's local URL is configured to
   point to `/home/tmikov/work/hermes-n-api`, so `git fetch` pulls directly
   from the local checkout — no push to GitHub needed:
   ```
   cd hermes && git fetch && git checkout <commit> && cd ..
   ```
4. Rebuild and continue your task with the fix in place.

### If the upstream fix fails

If the subagent cannot fix the bug (too complex, deep in VM internals, etc.):

1. Implement a workaround in this repo instead.
2. Document the bug clearly in `history/initial/memory.md` under
   "Hermes NAPI Bugs/Workarounds" — include: what the bug is, how to
   reproduce it, and what workaround was used.
3. Continue with your task.
