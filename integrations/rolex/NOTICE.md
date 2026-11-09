# ROLEX optional module provenance

Official source: https://github.com/iotlpf/ROLEX, commit 016a397 (FAST 2023).
License: GPL-3.0; see LICENSE. Enabling RRL_ENABLE_ROLEX links this GPL module;
an optional build does not remove the license obligations of that executable.

PLR comes from upstream rolex/plr.hpp. The fixed 64-slot leaf is a reduced
adapter of upstream rolex/leaf.hpp, with explicit occupancy for UINT64_MAX,
initialized slots, atomic values and standard C++ types replacing r2 helpers.
Adapter and mmap helpers originate in the inspected fuxian reproduction.
Changes add common lifecycle/neighbor APIs, pre-window build, configurable
segment/arena geometry and failure-atomic batch validation.

This is a local algorithm adapter. It has no remote learned-index directory,
RDMA leaf path, shadow redirection or service-side computation. It must not be
reported as a full ROLEX RDMA reproduction. Fixed models and a configurable
segment size are adaptations. Default core build excludes this module.
