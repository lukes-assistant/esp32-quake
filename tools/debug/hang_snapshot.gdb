# Attach to a running (possibly hung) board over the built-in USB-JTAG without resetting it,
# dump both cores and the inter-core pipeline state, then let it run again.
#   openocd -f board/esp32p4-builtin.cfg            (in one shell)
#   riscv32-esp-elf-gdb -batch -x tools/debug/hang_snapshot.gdb build/quake.elf
set pagination off
set confirm off
set remotetimeout 10
target extended-remote :3333
# OpenOCD halts both harts when GDB attaches
echo \n===== threads (FreeRTOS tasks) =====\n
info threads
echo \n===== backtraces of every task =====\n
thread apply all bt 12
echo \n===== pipeline / job queue state =====\n
printf "r_pipe pending=%d recframe=%d\n", pending, recframe
printf "jobs: numjobs=%d nextjob=%d closed=%d active=%d frame_gen=%u done_gen=%u worker_waiting=%d\n", numjobs, nextjob, jobs_closed, jobs_active, frame_gen, done_gen, worker_waiting
printf "bandcopy: started=%u done=%u\n", copies_started, copies_done
printf "cls.state=%d demoplayback=%d signon=%d loading=%d\n", cls.state, cls.demoplayback, cls.signon, scr_disabled_for_loading
detach
quit
