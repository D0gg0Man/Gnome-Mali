sudo grep -E "hwc2|hybris_wrapper|hwcomposer|libhybris|hwcomposerwindow" /proc/$(pgrep -n gnome-shell)/maps | awk '{print $NF}' | sort -u
echo "---"
grep "present err=0 fence=" /tmp/gnome.log | tail -5
echo "---"
grep -c "present err=0 fence=" /tmp/gnome.log
echo "--- HWC2 HAL ---"
ls -la /vendor/lib64/hw/hwcomposer.mt6877.so
echo "--- HWC2 binder ---"
ls -la /dev/hwbinder
