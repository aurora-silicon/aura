#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

from construct import *
from aura.constructutils import ConstructClass

__all__ = []

DRM_COMMAND_BASE = 0x40

AURORA_BO_PIPELINE = 1

class drm_aurora_submit_t(ConstructClass):
    subcon = Struct(
        "cmdbuf" / Int64ul,
        "in_syncs" / Int64ul,
        "in_sync_count" / Int32ul,
        "out_sync" / Int32ul,
    )

class drm_aurora_wait_bo_t(ConstructClass):
    subcon = Struct(
        "handle" / Int32ul,
        Padding(4),
        "timeout_ns" / Int64sl,
    )

class drm_aurora_create_bo_t(ConstructClass):
    subcon = Struct(
        "size" /  Int32ul,
        "flags" / Int32ul,
        "handle" / Int32ul,
        Padding(4),
        "offset" / Int64ul,
    )

#class drm_aurora_mmap_bo_t(ConstructClass):
    #subcon = Struct(
        #"handle" / Int32ul,
        #"flags" / Int32ul,
        #"offset" / Int64ul,
    #)

class drm_aurora_get_param_t(ConstructClass):
    subcon = Struct(
        "param" / Int32ul,
        Padding(4),
        "value" / Int64ul,
    )

class drm_aurora_get_bo_offset_t(ConstructClass):
    subcon = Struct(
        "handle" / Int32ul,
        Padding(4),
        "offset" / Int64ul,
    )

AURORA_MAX_ATTACHMENTS = 16

AURORA_ATTACHMENT_C  = 0
AURORA_ATTACHMENT_Z  = 1
AURORA_ATTACHMENT_S  = 2

class drm_aurora_attachment_t(ConstructClass):
    subcon = Struct(
        "type" / Int32ul,
        "size" / Int32ul,
        "pointer" / Int64ul,
    )

AURORA_CMDBUF_LOAD_C = (1 << 0)
AURORA_CMDBUF_LOAD_Z = (1 << 1)
AURORA_CMDBUF_LOAD_S = (1 << 2)

class drm_aurora_cmdbuf_t(ConstructClass):
    subcon = Struct(
        "flags" / Int64ul,

        "encoder_ptr" / Int64ul,
        "encoder_id" / Int32ul,

        "cmd_ta_id" / Int32ul,
        "cmd_3d_id" / Int32ul,

        "ds_flags" / Int32ul,
        "depth_buffer" / Int64ul,
        "stencil_buffer" / Int64ul,

        "scissor_array" / Int64ul,
        "depth_bias_array" / Int64ul,

        "fb_width" / Int32ul,
        "fb_height" / Int32ul,

        "load_pipeline" / Int32ul,
        "load_pipeline_bind" / Int32ul,

        "store_pipeline" / Int32ul,
        "store_pipeline_bind" / Int32ul,

        "partial_reload_pipeline" / Int32ul,
        "partial_reload_pipeline_bind" / Int32ul,

        "partial_store_pipeline" / Int32ul,
        "partial_store_pipeline_bind" / Int32ul,

        "depth_clear_value" / Float32l,
        "stencil_clear_value" / Int8ul,
        Padding(3),

        "attachments" / Array(AURORA_MAX_ATTACHMENTS, drm_aurora_attachment_t),
        "attachment_count" / Int32ul,
    )

__all__.extend(k for k, v in globals().items()
               if ((callable(v) or isinstance(v, type)) and v.__module__ == __name__) or isinstance(v, int))
