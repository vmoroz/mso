# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.

liblet_sources(
  SOURCES
    looperScheduler.cpp
    queueService.cpp
    queueService.h
    taskBatch.cpp
    taskBatch.h
    taskContext.cpp
    taskContext.h
    taskQueue.cpp
    taskQueue.h
    threadMutex.h
    uiSchedulerStub.h
  SOURCES_LINUX
    threadPoolScheduler_linux.cpp
  SOURCES_WIN
    threadPoolScheduler_win.cpp
  SOURCES_WINRT
    uiScheduler_winrt.cpp
)
