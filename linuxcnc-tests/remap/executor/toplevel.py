import hal
import remap


def __init__(self):
    remap.init_stdglue(self)
    component = hal.component("remap") if self.task else None
    self.chuck = remap.HalExecutor(self, "chuck", component)
    self.tailstock = remap.HalExecutor(self, "tailstock", component)
    self.memory_executor = remap.MemoryExecutor()

    if component is not None:
        component.newpin("owner-marker", hal.Type.BOOL, hal.Dir.OUT)
        component.newpin("raw-result", hal.Type.SINT, hal.Dir.OUT)
        component.newpin("transport-result", hal.Type.SINT, hal.Dir.OUT)
        component["owner-marker"] = True
        component.ready()
    self.executor_component = component
