#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 tobagin
# SPDX-License-Identifier: AGPL-3.0-only
#
# Close-cycle check for a console run started with run-desktop.sh --console and left at HOME
# (lock screen dismissed, cursor on the first tile; HOME wraps a LEFT on the first tile to the last,
# and puts the cursor back on the first tile after a close):
#
#   check-close.py <run> [--tile K] [--cycles N] [--play SECONDS]
#
# Each cycle launches the K-th tile (HOME keeps the most recently played first, so the cycles
# rotate through the first K+1 titles), lets it run, closes it from HOME (HOME > X > Close) and
# logs the process memory before launch, in game and after the close. An application that ends by
# itself (an abort) is accepted when HOME is back; its error dialog is dismissed.
# Pass: every cycle is back at HOME (qlaunch "Now running"), the run is alive, and the log shows
# "nvcore session N released" for each closed application. Exit 1 on the first failure.
import argparse
import os
import subprocess
import sys
import time

SCR = '~/REPOS/Openpak/scratch/harbor-eden'
DRIVE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'drive.sh')
HOME_ID = '0100000000001000'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--tile', type=int, default=4)
    ap.add_argument('--cycles', type=int, default=10)
    ap.add_argument('--play', type=float, default=75)
    args = ap.parse_args()
    run_dir = os.path.join(SCR, 'run', args.run)
    log_path = os.path.join(run_dir, 'user', 'log', 'eden_log.txt')
    pid = int(open(os.path.join(run_dir, 'pid')).read())

    def alive():
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def drive(*cmd):
        subprocess.run([DRIVE, args.run, *map(str, cmd)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    def log_lines():
        with open(log_path, 'rb') as f:
            return [line.decode(errors='replace') for line in f]

    def now_running():
        last = ''
        for line in log_lines():
            if 'Now running:' in line:
                last = line.split('Now running:')[1].strip()
        return last

    def released_count():
        return sum(1 for line in log_lines() if 'released (' in line and '~NVDRV' in line)

    def mem(label):
        status = {}
        for line in open(f'/proc/{pid}/status'):
            key, _, value = line.partition(':')
            status[key] = value.split()[0] if value.split() else ''
        print(f'{time.strftime("%H:%M:%S")} {label}: rss={status.get("VmRSS")} '
              f'anon={status.get("RssAnon")} shmem={status.get("RssShmem")} '
              f'swap={status.get("VmSwap")} KiB', flush=True)

    def fail(message):
        print(f'FAIL: {message}', flush=True)
        sys.exit(1)

    def wait_alive(seconds, what):
        end = time.time() + seconds
        while time.time() < end:
            if not alive():
                fail(f'the run died {what}')
            time.sleep(1)

    mem('start')
    for n in range(1, args.cycles + 1):
        nav = []
        for _ in range(args.tile):
            nav += ['press', 'RIGHT', '300', 'sleep', '1.0']
        released_before = released_count()
        drive(*nav, 'press', 'A', '300', 'sleep', '8')
        if now_running().startswith(HOME_ID):
            drive('press', 'A', '300', 'sleep', '8')
        title = now_running()
        if title.startswith(HOME_ID) or not title:
            fail(f'cycle {n}: nothing launched')
        print(f'cycle {n}: launched {title}', flush=True)
        wait_alive(args.play, 'while the application ran')
        mem(f'cycle {n} in game')
        if now_running().startswith(HOME_ID):
            print(f'cycle {n}: the application ended by itself', flush=True)
            drive('press', 'A', '300', 'sleep', '3')
        else:
            # HOME reads its pad once per frame and draws slowly while the application suspends,
            # so the presses are long; a dropped X (no dialog) leaves A on Continue, which only
            # resumes the application, and the close is tried again.
            drive('home', 'sleep', '8')
            for _attempt in range(3):
                drive('press', 'X', '600', 'sleep', '3', 'press', 'A', '600')
                wait_alive(30, 'after the close')
                if now_running().startswith(HOME_ID):
                    break
                drive('home', 'sleep', '8')
        if not now_running().startswith(HOME_ID):
            fail(f'cycle {n}: HOME is not back')
        if released_count() <= released_before:
            fail(f'cycle {n}: no nvdrv session was released')
        mem(f'cycle {n} after close')
    print('PASS', flush=True)


if __name__ == '__main__':
    main()
