# optional tuning:
export FADV_SMALL_CUTOFF=$((1<<20))   # 1 MiB
export FADV_OPEN_HINT=noreuse         # noreuse or none
export FADV_CLOSE_DROP=1              # 1 to drop at close, 0 to skip

LD_PRELOAD=$PWD/libfadv_drop.so make -j$(nproc)
