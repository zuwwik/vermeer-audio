# SPDX-License-Identifier: BSD-3-Clause
# Vermeer Android endpoint parity: logger -> MFC -> legacy LPAIF TDM_SINK.
# See android-reference/tdm-endpoint.txt for ACDB payloads and provenance.
include(`audioreach/audioreach.m4') dnl
include(`audioreach/subgraph.m4') dnl
include(`audioreach/container.m4') dnl
include(`audioreach/module_log.m4') dnl
include(`audioreach/module_mfc.m4') dnl

undefine(`LOG_MODULE_IID') dnl
undefine(`MFC_MODULE_IID') dnl
undefine(`TDM_MODULE_IID') dnl
undefine(`SG_INDEX') dnl
undefine(`CONTAINER_INDEX') dnl
undefine(`MOD_INDEX') dnl
undefine(`DEVICE_PERF_MODE') dnl
undefine(`DEVICE_DIRECTION') dnl
undefine(`CONT_CAP') dnl
undefine(`CONT_POSITION') dnl
undefine(`DEVICE_DOMAIN_ID') dnl
undefine(`CONT_STACK_SIZE') dnl

define(`DEVICE_PERF_MODE', APM_SG_PERF_MODE_LOW_LATENCY) dnl
define(`DEVICE_DIRECTION', APM_SUB_GRAPH_DIRECTION_TX) dnl
define(`CONT_CAP', `ifelse(LPASS_VER_COMPAT, 110000, APM_CONTAINER_TYPE_ID_GC, APM_CONTAINER_CAP_ID_EP)') dnl
define(`CONT_POSITION', APM_CONT_GRAPH_POS_STREAM) dnl
define(`DEVICE_DOMAIN_ID', ifdef(`AR_SELECTED_PROC_DOMAIN_ID', `AR_SELECTED_PROC_DOMAIN_ID', `APM_PROC_DOMAIN_ID_ADSP')) dnl
define(`CONT_STACK_SIZE', 8192) dnl
define(`LOG_MODULE_IID', MOD_IID_START) dnl
define(`MFC_MODULE_IID', eval(MOD_IID_START + 1)) dnl
define(`TDM_MODULE_IID', eval(MOD_IID_START + 2)) dnl
define(`SG_INDEX', 1) dnl
define(`CONTAINER_INDEX', 1) dnl
define(`MOD_INDEX', 1) dnl

dnl The upstream M4 token names use U32 for all word tuples. Their numeric
dnl IDs 262..269 and 272 are the kernel SYNC/SLOT/ACTIVE_LANE tokens.
dnl Legacy TDM sync_mode=1 means LONG, unlike the newer QAIF enumeration.
define(`AR_MODULE_VERMEER_TDM_RX',
`'
`SectionVendorTuples."NAME_PREFIX.tdm_rx$1_tuples" {'
`        tokens "audioreach_tokens"'
`        tuples."word.u32_data" {'
`                AR_TKN_U32_MODULE_INSTANCE_ID STR($4)'
`                AR_TKN_U32_MODULE_ID "0x0700100E"'
`                AR_TKN_U32_MODULE_MAX_IP_PORTS "1"'
`                AR_TKN_U32_MODULE_MAX_OP_PORTS "0"'
`                AR_TKN_U32_MODULE_SRC_INSTANCE_ID STR($4)'
`                AR_TKN_U32_MODULE_HW_IF_TYPE STR(DEVICE_INTF_TYPE)'
`                AR_TKN_U32_MODULE_HW_IF_IDX STR(DEVICE_INTF_INDEX)'
`                AR_TKN_U32_MODULE_FMT_DATA STR(DEVICE_DATA_FORMAT)'
`                AR_TKN_U32_MODULE_FRAME_SZ_FACTOR "1"'
`                AR_TKN_U32_MODULE_SYNC_SRC "1"'
`                AR_TKN_U32_MODULE_CTRL_DATA_OUT_ENABLE "1"'
`                AR_TKN_U32_MODULE_SLOT_MASK "3"'
`                AR_TKN_U32_MODULE_NSLOTS_PER_FRAME "4"'
`                AR_TKN_U32_MODULE_SLOT_WIDTH "32"'
`                AR_TKN_U32_MODULE_SYNC_MODE "1"'
`                AR_TKN_U32_MODULE_CTRL_INVERT_SYNC_PULSE "0"'
`                AR_TKN_U32_MODULE_CTRL_SYNC_DATA_DELAY "1"'
`                AR_TKN_U32_MODULE_ACTIVE_LANE_MASK "2"'
`        }'
`}'
`SectionData."NAME_PREFIX.tdm_rx$1_data" {'
`        tuples "NAME_PREFIX.tdm_rx$1_tuples"'
`}'
`SectionWidget."NAME_PREFIX.tdm_rx$1" {'
`        index STR($1)'
`        type "aif_in"'
`        no_pm "true"'
`        stream_name "DEVICE_NAME Playback"'
`        subseq "10"'
`        data ['
`                "NAME_PREFIX.sub_graph$2_data"'
`                "NAME_PREFIX.container$3_data"'
`                "NAME_PREFIX.tdm_rx$1_data"'
`        ]'
`}') dnl

AR_SUBGRAPH(SG_INDEX, DEVICE_DAI_ID, SG_IID_START, DEVICE_PERF_MODE, DEVICE_DIRECTION, APM_SUB_GRAPH_SID_AUDIO_PLAYBACK)
AR_CONTAINER(CONTAINER_INDEX, CONT_IID_START, CONT_CAP, CONT_STACK_SIZE, CONT_POSITION, DEVICE_DOMAIN_ID)
AR_MODULE_LOG(MOD_INDEX, SG_INDEX, CONTAINER_INDEX, LOG_MODULE_IID, 1, 1, 1, 2, 0x000019ab, 1, 0, MFC_MODULE_IID)
AR_MODULE_MFC(MOD_INDEX, SG_INDEX, CONTAINER_INDEX, MFC_MODULE_IID, 1, 1, 1, 2, TDM_MODULE_IID)
AR_MODULE_VERMEER_TDM_RX(MOD_INDEX, SG_INDEX, CONTAINER_INDEX, TDM_MODULE_IID)

SectionGraph."NAME_PREFIX.1 Vermeer TDM Graph" {
        index "1"
        lines [
                "NAME_PREFIX.logger1, , DEVICE_NAME Audio Mixer"
                "NAME_PREFIX.mfc1, , NAME_PREFIX.logger1"
                "NAME_PREFIX.tdm_rx1, , NAME_PREFIX.mfc1"
                "DEVICE_NAME Playback, , NAME_PREFIX.tdm_rx1"
        ]
}
