import shutil
from pathlib import Path

Import("env")


def copy_bin_to_dist(source, target, env):
    project_dir = Path(env['PROJECT_DIR'])
    dist_dir = project_dir / 'dist'
    dist_dir.mkdir(exist_ok=True)

    build_dir = Path(env.subst('$BUILD_DIR'))
    firmware_candidates = [
        build_dir / 'firmware.bin',
        build_dir / 'program.bin',
    ]

    firmware = next((p for p in firmware_candidates if p.exists()), None)
    if firmware is None:
        print('Warning: no generated firmware.bin found for Launcher packaging')
        return

    launcher_bin = dist_dir / 'launcher-esp32-2432s028r.bin'
    shutil.copy2(firmware, launcher_bin)
    print(f'Launcher package created: {launcher_bin}')


env.AddPostAction('$BUILD_DIR/firmware.bin', copy_bin_to_dist)
env.AddPostAction('$BUILD_DIR/program.bin', copy_bin_to_dist)
