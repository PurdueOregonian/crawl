-- Not run by the normal suite (test/big/). Deliberately crashes, to check
-- that a hard crash (access violation) still writes a crash report with a
-- symbolised stack trace. Run: crawl -test big/segfault, then look for the
-- newest morgue/crash-*.txt naming debug_segfault.
debug.segfault()
