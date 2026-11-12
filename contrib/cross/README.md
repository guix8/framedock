# PC에서 빌드해 헤드셋에 설치하기 (크로스 빌드)

Steam Frame(aarch64 SteamOS)용 Frame Dock을 PC에서 빌드합니다. 테스트도 PC에서 돌리고, 헤드셋에는 결과 파일만 복사해 설치합니다. 헤드셋에서는 컴파일하지 않습니다.

이렇게 하는 이유가 있습니다. 2026-11-09에 착용 중인 헤드셋에서 `cmake -j2` 빌드를 돌리는 동안 헤드셋이 재부팅됐습니다. CPU는 95°C를 넘었고, PMIC 기록은 SMPL(순간 전원 손실)이었습니다. 배터리는 약 3A를 쓰고 있었습니다. PC 빌드는 이런 부하를 헤드셋에서 없애고, 속도도 훨씬 빠릅니다(전체 빌드 약 1분, 헤드셋 `-j2`로는 수 분).

다른 aarch64 SteamOS 프로젝트에도 그대로 쓸 수 있게 일반적인 부분과 Frame Dock 전용 부분을 나눠 적습니다.

## 구성

| 파일 | 하는 일 | 다른 프로젝트에서 |
|---|---|---|
| `aarch64-sysroot.cmake` | CMake 툴체인입니다. clang과 lld로 헤드셋의 sysroot에 맞춰 빌드하고, pkg-config도 sysroot를 보게 합니다. qemu가 있으면 테스트를 qemu로 실행합니다 | 그대로 복사해 씁니다. `OPENVR_LIB_DIR` 줄만 Frame Dock 전용입니다 |
| `make-sysroot.sh` | 헤드셋에서 헤더, C/C++ 런타임, GCC 파일, 실행 파일이 쓰는 공유 라이브러리, pkg-config, 글꼴을 PC로 복사합니다 | 인자로 자기 프로그램 경로를 주거나 `EXTRA=`로 라이브러리를 추가합니다 |
| `deploy-app.sh` | PC에서 빌드한 `framedock`을 릴리스 묶음과 같은 모양(`bin/`, `contrib/`)으로 ssh로 보냅니다. 그다음 헤드셋에서 `install.sh --app-only --no-restart`를 실행합니다 | 설치 단계만 자기 프로젝트에 맞게 바꿉니다 |

## 1회 준비 (Windows + WSL)

이 PC에서는 WSL `Ubuntu-24.04`를 씁니다. Linux PC라면 WSL 없이 그대로 하면 됩니다.

```bash
sudo apt-get update
sudo apt-get install -y clang lld cmake ninja-build pkg-config qemu-user
```

clang 18과 lld 18로 확인했습니다. GCC 크로스 컴파일러는 필요 없습니다. clang이 sysroot 안의 헤드셋 GCC 15(libstdc++ 헤더, crt 파일)를 그대로 씁니다.

헤드셋 sysroot는 약 0.9GB입니다. 라이브러리와 헤더가 약 0.5GB, 글꼴이 약 0.4GB입니다.

```bash
# WSL 안에서. Windows의 ssh와 키를 쓰려면 SSH=ssh.exe를 붙입니다(WSL에 키를 복사하지 않음)
SSH=ssh.exe sh contrib/cross/make-sysroot.sh steamos@<headset-ip>
```

헤드셋은 읽기만 합니다(nice 19, ionice idle, 압축 없음). 그래도 사용자가 착용하고 있지 않을 때 하는 것이 좋습니다. **SteamOS를 업데이트한 뒤에는 다시 실행합니다.** 라이브러리 버전이 바뀌면 PC 빌드 결과가 헤드셋에서 실행되지 않을 수 있습니다.

## 빌드와 테스트

```bash
cmake -S . -B build-cross -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=contrib/cross/aarch64-sysroot.cmake -DFRAME_SYSROOT=$HOME/frame-sysroot
ninja -C build-cross
ctest --test-dir build-cross -j8 -E browser-session
```

- 테스트는 qemu-user가 aarch64 실행 파일을 PC에서 에뮬레이션해 돌립니다(`CMAKE_CROSSCOMPILING_EMULATOR`). 테스트 실행 파일을 직접 실행할 때는 `QEMU_LD_PREFIX=$HOME/frame-sysroot ./build-cross/gaze-test`처럼 실행합니다.
- 2026-11-09 결과(`ef732db`): 38개 중 37개가 통과했습니다. 다른 결과는 다음과 같습니다.
  - `device-services`: 없는 프로그램을 실행했을 때 나오는 오류 문구가 qemu에서 다릅니다. 헤드셋에서는 통과합니다.
  - `browser-session`: 에뮬레이션에서는 7분 넘게 걸리고 시간 초과로 실패합니다. 헤드셋에서는 약 50초에 통과합니다.
  - 이 두 개와 실제 SteamVR 동작은 헤드셋에서 확인합니다. 헤드셋을 벗어 둔 상태에서 `ctest -R 'device-services|browser-session'`만 짧게 돌리면 됩니다.
- 글꼴을 sysroot에 넣기 전에는 글자 폭을 재는 테스트 3개(layout-editor, battery-media, color-page)가 실패했습니다. 지금은 `make-sysroot.sh`가 헤드셋의 글꼴과 fontconfig 설정을 함께 복사합니다.
- clang은 GCC보다 경고를 몇 개 더 냅니다(`-Wunused-private-field` 등, 기존 코드). 헤드셋 GCC 빌드의 경고 기준은 그대로입니다.

## 헤드셋에 설치

```bash
SSH=ssh.exe sh contrib/cross/deploy-app.sh steamos@<headset-ip>
```

앱만 바꾸고 SteamVR은 재시작하지 않습니다. 끝에 설치된 파일의 sha256 앞부분이 출력됩니다.

2026-11-09에 확인했습니다. PC에서 빌드한 `d23d508`을 이 스크립트로 설치했고, 헤드셋에서 정상 실행됐습니다(시선 소스·격자·90Hz 로그 확인). 설치기가 마지막에 "Remove: sh /tmp/framedock-deploy.…/uninstall.sh"라고 알려 주는데, 그 임시 폴더는 설치가 끝나면 지워집니다. 제거하려면 저장소의 `contrib/uninstall.sh`를 헤드셋으로 복사해 실행합니다.

Vulkan 레이어(`third_party/vkbasalt`, meson)는 아직 PC 빌드를 만들지 않았습니다. 레이어를 바꿀 때는 meson cross 파일이 필요하고, 레이어를 바꾸면 SteamVR을 재시작해야 합니다. 그때까지 레이어는 헤드셋 빌드(`contrib/install.sh`)로 설치합니다. 이 빌드는 헤드셋을 벗어 두고 온도를 확인한 뒤에 합니다.

## 다른 프로젝트에 쓰기

1. `aarch64-sysroot.cmake`를 복사합니다. 프로젝트 전용 줄(`OPENVR_LIB_DIR`)은 지우거나 바꿉니다.
2. `make-sysroot.sh <host> /path/to/your/program`을 실행합니다. 아직 헤드셋에 프로그램이 없으면, 필요한 라이브러리를 `EXTRA="usr/lib/libfoo.so* usr/lib/libbar.so*"`로 지정합니다. 링크할 때 필요한 `.so` 심볼릭 링크도 패턴에 포함되게 합니다.
3. CMake가 아닌 프로젝트는 같은 옵션을 직접 줍니다: `clang --target=aarch64-unknown-linux-gnu --sysroot=$HOME/frame-sysroot -fuse-ld=lld`. pkg-config에는 `PKG_CONFIG_SYSROOT_DIR`과 `PKG_CONFIG_LIBDIR`를 설정합니다(툴체인 파일 참고).
4. 실행 파일이 헤드셋에서 `GLIBC_2.xx not found` 같은 오류를 내면 sysroot가 오래된 것입니다. 2단계를 다시 합니다.
