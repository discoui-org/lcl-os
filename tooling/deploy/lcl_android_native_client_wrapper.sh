#!/System/Tools/bash
set -e

export ANDROID_DATA=/data
export LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64/bionic:/system/lib64:/vendor/lib64:/system_ext/lib64

case "$0" in
    */lcl-desktop-shell)
        exec /AndroidClients/lcl-desktop-shell "$@"
        ;;
    */lcl-mobile-shell)
        exec /AndroidClients/lcl-mobile-shell "$@"
        ;;
    */Terminal)
        exec /AndroidClients/lcl-terminal "$@"
        ;;
    */lcl-js)
        exec /AndroidClients/lcl-js "$@"
        ;;
    *)
        echo "Unsupported Android-native LCL client wrapper target: $0" >&2
        exit 64
        ;;
esac
