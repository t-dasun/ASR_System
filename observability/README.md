# Observability

M4 implements vendor-independent call timing/type-7 distributions and `ISystemSampler` with a Linux `/proc` implementation. `ResourceMonitor` samples the controller/native worker on a separate thread with a bounded buffer; required measurement loss invalidates a run. Simulated-clock tests skip real resource sampling. See [M4 metric definitions](../docs/m4-code.md). Server telemetry and dashboard overhead remain later work.
