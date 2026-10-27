#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual kernel parser, packets, replies and DMA cleanup under ASan.

Only host allocations and a simulated command transport are used. This does
not validate DSP support, modem interaction, audio quality or a real call.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(os.environ.get("KERNEL_SOURCE", Path(__file__).resolve().parents[2] / "vermeer-mainline"))
SOURCE = (ROOT / "sound/soc/qcom/qdsp6/q6apm-voice.c").read_text()
AR = (ROOT / "sound/soc/qcom/qdsp6/audioreach.c").read_text()
HEADER = (ROOT / "sound/soc/qcom/qdsp6/audioreach.h").read_text()
APR = (ROOT / "include/linux/soc/qcom/apr.h").read_text()


def definition(source, name):
    return re.search(r"struct " + re.escape(name) + r"\s*\{.*?\}\s*(?:__packed)?;", source, re.S)[0]


def function(source, name):
    start = re.search(r"^(?:static )?[^\n]*\b" + re.escape(name) + r"\(", source, re.M).start()
    # These kernel functions' outer closing brace is at column zero.
    end = source.index("\n}", start) + 2
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware_directory", type=Path)
    args = parser.parse_args()
    harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define __packed __attribute__((packed))
#define __free(x) __attribute__((cleanup(free_pointer)))
#define guard(x) (void)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define IS_ERR(p) (!(p))
#define PTR_ERR(p) (-ENOMEM)
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define SZ_4K 4096
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define le32_to_cpu(x) (x)
#define lower_32_bits(x) ((uint32_t)(x))
#define upper_32_bits(x) ((uint32_t)((uint64_t)(x) >> 32))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define dev_err(...) ((void)0)
#define GPR_PKT_VER 0
#define GPR_PKT_HEADER_WORD_SIZE 6
#define GPR_DOMAIN_ID_ADSP 2
#define GPR_DOMAIN_ID_APPS 3
#define GPR_BASIC_RSP_RESULT 0x02001005
#define APM_PORT_MAX 256
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
typedef uint64_t dma_addr_t;
typedef int spinlock_t;
struct mutex { int dummy; };
struct completion { unsigned int count; };
typedef struct { u32 id; } gpr_port_t;
struct firmware { const u8 *data; size_t size; };
struct audioreach_module {
    u32 instance_id, hw_interface_type, hw_interface_idx, data_format;
};
struct audioreach_module_config { u32 sample_rate, bit_width, num_channels; };
static void free_pointer(void *pointer) { free(*(void **)pointer); }
static u32 get_unaligned_le32(const void *pointer) { u32 value; memcpy(&value,pointer,4); return value; }
static void complete(struct completion *done) { done->count++; }
static int allocations, frees, releases, module_puts;
static void *dma_alloc_coherent(void *dev, size_t size, dma_addr_t *address, int flags)
{
    void *result=calloc(1,size); (void)dev; (void)flags;
    if(result) { *address=(uintptr_t)result; allocations++; } return result;
}
static void dma_free_coherent(void *dev, size_t size, void *buffer, dma_addr_t address)
{ (void)dev; (void)size; (void)address; frees++; free(buffer); }
static void release_firmware(const struct firmware *firmware) { (void)firmware; releases++; }
static void module_put(void *module) { (void)module; module_puts++; }
static bool test_bit(unsigned int bit, unsigned long *value) { return !!(*value & (1UL<<bit)); }
static void set_bit(unsigned int bit, unsigned long *value) { *value |= 1UL<<bit; }
static void clear_bit(unsigned int bit, unsigned long *value) { *value &= ~(1UL<<bit); }
'''
    for source in (SOURCE, HEADER):
        harness += "\n" + "\n".join(re.findall(r"^#define (?:APM_VOICE_\w+|VCPM_\w+|VOICE_MAX_\w+|PARAM_ID_(?:HW_EP_MF_CFG|CODEC_DMA_INTF_CFG|HW_EP_POWER_MODE_CFG|VOL_CTRL_MASTER_GAIN)|APM_(?:CMD_\w+|MODULE_INSTANCE_ID|PARAM_ID_SUB_GRAPH_LIST|MEMORY_MAP_SHMEM8_4K_POOL))\s+[^\n]+", source, re.M))
    # re.findall above omits the directive intentionally; add it to each definition.
    harness = re.sub(r"^(APM_|VCPM_|VOICE_|PARAM_ID_)([^\n]+)$", r"#define \1\2", harness, flags=re.M)
    for name in ("gpr_hdr", "gpr_pkt", "gpr_resp_pkt", "gpr_ibasic_rsp_result_t"):
        harness += "\n" + definition(APR, name)
    for name in ("apm_cmd_header", "apm_module_param_data", "apm_cmd_shared_mem_map_regions",
                 "apm_shared_map_region_payload", "apm_cmd_shared_mem_unmap_regions",
                 "param_id_hw_ep_mf", "param_id_codec_dma_intf_cfg"):
        harness += "\n" + definition(HEADER, name)
    harness += "\n#define GPR_HDR_SIZE sizeof(struct gpr_hdr)\n#define APM_CMD_HDR_SIZE sizeof(struct apm_cmd_header)\n"
    for name in ("voice_fw_header", "voice_fw_path", "q6apm_voice_path", "q6apm_voice"):
        harness += "\n" + definition(SOURCE, name)
    for name in ("__audioreach_alloc_pkt", "audioreach_alloc_apm_pkt", "audioreach_alloc_cmd_pkt"):
        code = function(AR, name).replace("kzalloc(pkt_size, GFP_KERNEL)", "calloc(1, pkt_size)")
        code = code.replace("ERR_PTR(-ENOMEM)", "NULL")
        harness += "\n" + code
    harness += r'''
static u32 fail_opcode;
static int failure;
static unsigned int commands;
static unsigned int registrations;
static unsigned int power_writes;
static u8 media_payload[12];
static struct gpr_pkt *last;
static int voice_send(struct q6apm_voice_path *path, struct gpr_pkt *packet)
{
    const struct apm_cmd_header *header=(void *)packet+GPR_HDR_SIZE;
    commands++;
    free(last); last=malloc(packet->hdr.pkt_size); assert(last);
    memcpy(last,packet,packet->hdr.pkt_size);
    if(packet->hdr.opcode==APM_CMD_SET_CFG && packet->hdr.pkt_size>=56) {
        const struct apm_module_param_data *param=(void *)packet+40;
        if(param->param_id==PARAM_ID_HW_EP_POWER_MODE_CFG) power_writes++;
        if(param->param_id==PARAM_ID_HW_EP_MF_CFG) {
            assert(param->param_size==sizeof(media_payload));
            memcpy(media_payload,param+1,sizeof(media_payload));
        }
    }
    if(packet->hdr.opcode==APM_VOICE_REGISTER_CFG) {
        uint64_t address=(uint64_t)header->payload_address_lsw | (uint64_t)header->payload_address_msw<<32;
        size_t offset=address-path->dsp_address;
        assert(offset>=path->persist_offset+8 && offset+header->payload_size<=path->buffer_size);
        assert(get_unaligned_le32(path->buffer+offset)==4);
        assert(get_unaligned_le32(path->buffer+offset+4)==0x08001163);
        assert(header->mem_map_handle==path->map_handle);
        assert(packet->hdr.pkt_size==40);
        registrations++;
    }
    return packet->hdr.opcode==fail_opcode ? failure : 0;
}
'''
    for name in ("voice_callback", "voice_oob", "voice_module_param", "voice_param",
                 "voice_capture_gain", "voice_volume", "voice_mgmt", "voice_valid_params",
                 "voice_take", "voice_parse_path", "voice_map", "voice_unmap",
                 "voice_persist", "voice_cleanup", "voice_codec_media_format"):
        harness += "\n" + function(SOURCE, name)
    harness += r'''
static void parse(struct q6apm_voice *voice, const u8 *data, size_t size)
{
    const struct voice_fw_header *header=(void *)data;
    const u8 *cursor=data+sizeof(*header), *end=data+size;
    static gpr_port_t ports[2]={{.id=0x10000001},{.id=0x10000002}};
    assert(!memcmp(header->magic,"ARVOICE\0",8) && header->version==3 && header->paths==2);
    for(int i=0;i<2;i++) {
        struct q6apm_voice_path *p=&voice->paths[i]; p->voice=voice; p->port=&ports[i];
        assert(voice_parse_path(p,&cursor,end)==0);
        p->dsp_address=0x100000000ULL+(uint64_t)i*0x100000; p->map_handle=0x1234+i;
    }
    assert(cursor==end); voice->vsid=0x11c05000;
}
static void replies(void)
{
    struct q6apm_voice_path path={.expected_opcode=APM_CMD_SET_CFG,.expected_token=7};
    struct gpr_ibasic_rsp_result_t result={.opcode=APM_CMD_SET_CFG};
    struct gpr_resp_pkt response={.hdr={.opcode=GPR_BASIC_RSP_RESULT,.token=6},
                                 .payload=&result,.payload_size=sizeof(result)};
    voice_callback(&response,&path,0); assert(!path.done.count);
    response.hdr.token=7; response.payload_size=-1;
    voice_callback(&response,&path,0); assert(!path.done.count);
    response.payload_size=7;
    voice_callback(&response,&path,0); assert(!path.done.count);
    response.payload_size=8;
    voice_callback(&response,&path,0); assert(path.done.count==1 && !path.expected_token);
    voice_callback(&response,&path,0); assert(path.done.count==1);
    response.hdr.token=0;
    voice_callback(&response,&path,0); assert(path.done.count==1);
    path.expected_token=8; path.expected_opcode=APM_CMD_SHARED_MEM_MAP_REGIONS;
    response.hdr.token=8; result.opcode=APM_CMD_SHARED_MEM_MAP_REGIONS;
    voice_callback(&response,&path,0); assert(path.done.count==1);
    response.hdr.opcode=APM_CMD_RSP_SHARED_MEM_MAP_REGIONS;
    u32 handle=77; response.payload=&handle; response.payload_size=4;
    voice_callback(&response,&path,0); assert(path.done.count==2 && path.map_handle==77);
}
int main(int argc, char **argv)
{
    assert(argc==2); FILE *file=fopen(argv[1],"rb"); assert(file);
    fseek(file,0,SEEK_END); long size=ftell(file); rewind(file);
    u8 *data=malloc(size); assert(data && fread(data,1,size,file)==(size_t)size); fclose(file);
    struct q6apm_voice voice={0}; parse(&voice,data,size); replies();
    assert(voice_volume(&voice,3)==0);
    struct apm_module_param_data *param=(void *)last+40;
    assert(last->hdr.dest_port==4 && param->param_id==VCPM_CAL_KEYS && param->param_size==32);
    assert(get_unaligned_le32((void *)(param+1)+12)==2); /* Android level: 100%-60%=2 */
    assert(voice_capture_gain(&voice,4096,false)==0);
    assert(last->hdr.dest_port==voice.paths[1].fw->gain_instance);
    assert(get_unaligned_le32((void *)last+44)==PARAM_ID_VOL_CTRL_MASTER_GAIN);
    assert(get_unaligned_le32((void *)last+56)==4096);
    assert(voice_capture_gain(&voice,4096,true)==0);
    assert(get_unaligned_le32((void *)last+56)==0);
    struct audioreach_module endpoint={.instance_id=0x43af,.hw_interface_type=1,.hw_interface_idx=4,.data_format=1};
    struct audioreach_module_config cfg={.sample_rate=48000,.bit_width=16,.num_channels=2};
    unsigned int count=commands;
    assert(voice_codec_media_format(&voice.paths[1],&endpoint,&cfg)==0);
    assert(commands==count+2 && power_writes==0);
    static const u8 expected_media[]={0x80,0xbb,0,0,0x10,0,2,0,1,0,0,0};
    assert(!memcmp(media_payload,expected_media,sizeof(media_payload)));
    param=(void *)last+40;
    assert(param->param_id==PARAM_ID_CODEC_DMA_INTF_CFG && param->param_size==12);
    assert(get_unaligned_le32(param+1)==1 && get_unaligned_le32((void *)(param+1)+4)==4);
    assert(get_unaligned_le32((void *)(param+1)+8)==3);
    for(int i=0;i<2;i++) { assert(voice_map(&voice.paths[i])==0); assert(voice_persist(&voice.paths[i],true)==0); }
    assert(registrations==5);
    voice.owned=true; for(int i=0;i<2;i++) voice.paths[i].opened=voice.paths[i].started=true;
    assert(voice_cleanup(&voice)==0 && !voice.owned && allocations==frees && module_puts==1);
    memset(&voice,0,sizeof(voice)); parse(&voice,data,size); voice.owned=true;
    int before=frees; fail_opcode=APM_CMD_SHARED_MEM_UNMAP_REGIONS; failure=-EREMOTEIO;
    assert(voice_cleanup(&voice)<0 && voice.poisoned && voice.owned && frees==before && module_puts==1);
    fail_opcode=0; voice.poisoned=false; assert(voice_cleanup(&voice)==0 && allocations==frees);
    memset(&voice,0,sizeof(voice)); parse(&voice,data,size); voice.owned=true;
    fail_opcode=APM_VOICE_REGISTER_CFG; failure=-ETIMEDOUT;
    assert(voice_persist(&voice.paths[0],true)==-ETIMEDOUT && voice.poisoned);
    before=frees; assert(voice_cleanup(&voice)<0 && frees==before);
    fail_opcode=0; voice.poisoned=false; assert(voice_cleanup(&voice)==0 && allocations==frees);
    /* Every truncated prefix of the first path must be rejected, including
       partial headers, lists and calibration blobs, without reading past it. */
    const u8 *first=data+sizeof(struct voice_fw_header);
    struct voice_fw_path saved; memcpy(&saved,first,sizeof(saved));
    size_t first_size=sizeof(saved)+4*(saved.num_subgraphs+saved.num_mfcs)+
                      saved.open_size+saved.cal_size+saved.persist_size;
    for(size_t n=0;n<first_size;n++) {
        const u8 *cursor=first; struct q6apm_voice_path p={.voice=&voice};
        assert(voice_parse_path(&p,&cursor,first+n)<0 && !p.buffer);
    }
    struct voice_fw_path *bad=(void *)first; bad->num_subgraphs=UINT32_MAX;
    const u8 *cursor=first; struct q6apm_voice_path p={.voice=&voice};
    assert(voice_parse_path(&p,&cursor,data+size)<0); *bad=saved;
    assert(allocations==frees); free(data); free(last); return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="audioreach-voice-test-") as tmp:
        source, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        source.write_text(harness)
        subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-Wall", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        str(source), "-o", str(exe)], check=True)
        for route in ("handset", "speaker"):
            subprocess.run([str(exe), str(args.firmware_directory / f"vermeer-voice-{route}.bin")],
                           check=True)
    print("PASS: both real ACDB images; every truncated first-path prefix rejected under ASan/UBSan")
    print("PASS: VCPM volume/mute packets; persistent table prefixes excluded")
    print("PASS: voice DMA format/interface packets retain calibrated XO policy")
    print("PASS: stale, duplicate, zero-token and short replies; MAP handle response")
    print("PASS: successful DMA teardown; failed unmap and registration timeout retain DSP memory")


if __name__ == "__main__":
    main()
