import os
import sys

# Make "ClaudeBridge" and "fake_live" importable.
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "..", "bridge"))
sys.path.insert(0, here)
