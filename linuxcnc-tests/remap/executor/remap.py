from stdglue import *


def m10_execute(self, **words):
    yield from executor_execute(self, self.chuck, request=0)


def m11_execute(self, **words):
    yield from executor_execute(self, self.chuck, request=1, timeout=0.2)


def m20_transact(self, **words):
    response = yield from executor_transact(self, self.tailstock, request=7)
    if not self.task:
        return
    self.executor_component["raw-result"] = response.result


def m21_non_hal(self, **words):
    response = yield from executor_execute(self, self.memory_executor, request=41)
    if not self.task:
        return
    if self.memory_executor.last_operation.released:
        self.executor_component["transport-result"] = response.result


class MemoryExecutor:
    name = "memory"

    def begin(self, request):
        self.last_operation = MemoryOperation(request)
        return self.last_operation


class MemoryOperation:
    def __init__(self, request):
        self.request = request
        self.polls = 0
        self.released = False
        self.cancelled = False

    def poll(self):
        self.polls += 1
        if self.polls == 1:
            return None
        return ExecutorResponse(EXECUTOR_SUCCEEDED, result=self.request + 1)

    def release(self):
        self.released = True

    def cancel(self):
        self.cancelled = True
