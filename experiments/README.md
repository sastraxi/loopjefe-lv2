# experiments/

Throwaway probes for tempo-follow design exploration. These are
**not** part of `make check` -- the real engine test suite (`tests/`)
is self-contained. Run them by hand:

```sh
cd experiments && make run
```

| File | What it answers |
|---|---|
| `wsola_proto.cpp` | The hand-rolled WSOLA voice probe: cross-wrap continuity, re-seed cleanliness, and CPU as a fraction of real-time. The result of this probe is now the production `src/wsola.h`; the prototype is kept as a self-contained, single-file reference of the earlier textbook-Hann/AMDF exploration. |

Validation scaffolding, not shipping code. Delete once the design
is stable and the in-process tests carry the relevant assertions.
