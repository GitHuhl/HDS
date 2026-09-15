# PyInstaller spec for a single-file Windows build of the frontend, with synth_engine.exe bundled
# alongside it as a data file. PyInstaller does not cross-compile -- this MUST be run on Windows
# itself (inside a venv with the packages from requirements.txt installed), after
# `make windows` has produced synth_engine.exe. See the README's "Building for Windows" section.
#
#   pyinstaller frontend.spec
#
# produces dist/HDS.exe -- one file, no separate synth_engine.exe or Python install needed by
# whoever runs it. frontend.py's own _maybe_launch_bundled_backend() (see src/frontend.py) detects
# it's running from a frozen PyInstaller bundle and launches the bundled synth_engine.exe
# automatically on startup.
import os

block_cipher = None
repo_root = os.path.dirname(os.path.abspath(SPEC))

a = Analysis(
    ["src/frontend.py"],
    pathex=[repo_root],
    binaries=[],
    datas=[
        (os.path.join(repo_root, "synth_engine.exe"), "."),
    ],
    hiddenimports=[],
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    cipher=block_cipher,
)
pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="HDS",
    console=False,
    onefile=True,
)
