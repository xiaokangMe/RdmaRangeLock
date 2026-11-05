"""Own only the processes created for one shared-data case; no global cleanup."""
import os
import json
from pathlib import Path
import select
import socket
import subprocess


def free_ports(count):
    """Claim `count` distinct ephemeral ports in one shot.

    Binding and closing one socket at a time lets the kernel hand the same
    number back for the next request, which is how one case ended up with two
    services on the same port. Keep every guard socket open until all ports are
    known, so the numbers are guaranteed to differ.
    """
    guards = []
    try:
        for _ in range(count):
            guard = socket.socket()
            guard.bind(("0.0.0.0", 0))
            guards.append(guard)
        return [guard.getsockname()[1] for guard in guards]
    finally:
        for guard in guards:
            guard.close()


class SharedServices:
    def __init__(self, build, config, logdir, env, native=None, data_binary="rrl_rdma_data_server"):
        self.build, self.config, self.logdir = Path(build), Path(config), Path(logdir)
        self.env, self.native = env, native
        self.data_binary=data_binary
        cfg=json.loads(self.config.read_text())
        self.leaf_objects=cfg.get("hierarchy_layout")=="leaf" or bool(cfg.get("native_object_locks",False))
        if self.leaf_objects and native is not True:raise ValueError("leaf object services require native mode")
        self.lock_budget=int(cfg.get("lock_budget",16384))
        if not 1<=self.lock_budget<=1048576:raise ValueError("lock_budget out of bounds")
        self.processes, self.logs = [], []
        self.data_port, self.lock_port, self.coordinator_port, self.txn_state_port = free_ports(4)
        if len({self.data_port, self.lock_port, self.coordinator_port, self.txn_state_port}) != 4:
            raise RuntimeError("ephemeral service port collision; no case started")

    def __enter__(self):
        self.logdir.mkdir(parents=True, exist_ok=True)
        specs = [(self.data_binary, ["--config", str(self.config), "--port", str(self.data_port)]),
                 ("rrl_rdma_lock_server", ([str(self.lock_port),"--objects",str(self.config)] if self.leaf_objects else [str(self.lock_port)] + (["--native-fail-stop"] if self.native is True else []))),
                 ]
        if self.native is not True:
            specs.append(("rrl_wait_coordinator", [str(self.coordinator_port), str(self.lock_budget), "600000"]))
        if self.native is not False:
            specs.append(("rrl_rdma_txn_server", [str(self.txn_state_port)]))
        try:
            for name, args in specs:
                log = (self.logdir / (name + ".log")).open("w")
                self.logs.append(log)
                p = subprocess.Popen([str(self.build / name)] + args, env=self.env,
                                     stdout=subprocess.PIPE, stderr=log, text=True)
                self.processes.append(p)
                if not select.select([p.stdout], [], [], 60)[0]:
                    raise RuntimeError(name + " readiness timeout")
                line = p.stdout.readline()
                log.write(line); log.flush()
                if "listening" not in line:
                    raise RuntimeError(name + " failed readiness; " + str(self.logdir))
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def check(self):
        for p in self.processes:
            if p.poll() is not None:
                raise RuntimeError("shared service exited: " + str(p.args))

    def test_env(self, host="127.0.0.1"):
        return dict(self.env, RRL_TEST_RDMA_HOST=host, RRL_TEST_RDMA_PORT=str(self.lock_port),
                    RRL_TEST_DATA_PORT=str(self.data_port), RRL_TEST_COORDINATOR_PORT=str(self.coordinator_port),
                    RRL_TEST_TXN_STATE_PORT=str(self.txn_state_port))

    def __exit__(self, *args):
        for p in reversed(self.processes):
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait(timeout=10)
            if p.stdout: p.stdout.close()
        for log in self.logs: log.close()
