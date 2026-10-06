## Produces a real Android SDK (platform-tools, build-tools 34.0.0, platform 34) + NDK
## 26.1.10909125, entirely through Nix substitution, with ZERO direct contact with
## dl.google.com -- see android/README.md for the full story. This is the exact expression that
## produced the SDK/NDK tools/build_android_manual.sh drives directly (aapt2/d8/apksigner/
## zipalign + NDK clang), bypassing Gradle/AGP's own (blocked, in this sandbox) download path
## entirely.
##
## Same pinned nixpkgs-24.05 release this project's own top-level shell.nix already uses -- see
## that file's own header comment for why the pin matters (reproducibility across machines/time),
## verbatim the same URL+hash so this and shell.nix never drift apart on nixpkgs revision.
##
## Needs NIXPKGS_ACCEPT_ANDROID_SDK_LICENSE=1 and NIXPKGS_ALLOW_UNFREE=1 in the environment (the
## build script sets both) -- the Android SDK's own license, nothing optional about accepting it
## to use Google's SDK/NDK at all, Nix-built or not.
{ pkgs ? import (builtins.fetchTarball {
    url = "https://releases.nixos.org/nixos/24.05/nixos-24.05.7376.b134951a4c9f/nixexprs.tar.xz";
    sha256 = "1f8j7fh0nl4qmqlxn6lis8zf7dnckm6jri4rwmj0qm1qivhr58lv";
  }) {} }:
(pkgs.androidenv.composeAndroidPackages {
  platformVersions = [ "34" ];
  buildToolsVersions = [ "34.0.0" ];
  includeNDK = true;
  ndkVersions = [ "26.1.10909125" ];
}).androidsdk
