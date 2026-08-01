# Fixture for decisions/ADR-003-caller-aware-import-gating.md's B14 regression test (added during
# this prove pass's own independent verification, not part of the ADR's original §4 claims table --
# see python_lockdown.cpp's g_importlib_dict comment for the finding this exercises).
#
# Loaded through the real finder -> TrustedLoaderProxy path (it is an ordinary, allowlisted
# top-level module, added to allowed_top_level_modules, not itself in caller_gated_modules), so its
# own module dict and top-level code object are registered trusted BEFORE this body executes
# (ADR-003 §3.3.2). Its top-level code then uses importlib.import_module() -- not the `import`
# statement -- to fetch a caller-gated name ("winreg") that the calling test asserts is NOT yet in
# sys.modules at this point, so this exercises a genuine cache-miss delegation through the REAL,
# captured import_module (g_real_import_module), not the wrapper's own fast-path check alone.
import importlib

RESULT = importlib.import_module("winreg")
