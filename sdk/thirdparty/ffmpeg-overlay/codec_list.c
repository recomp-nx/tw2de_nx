static const AVCodec* const codec_list[] = {
#if CONFIG_MP3FLOAT_DECODER
    &ff_mp3float_decoder,
#endif
#if CONFIG_MP3_DECODER
    &ff_mp3_decoder,
#endif
#if CONFIG_WMAPRO_DECODER
    &ff_wmapro_decoder,
#endif
#if CONFIG_WMAV2_DECODER
    &ff_wmav2_decoder,
#endif
#if CONFIG_XMAFRAMES_DECODER
    &ff_xmaframes_decoder,
#endif
#if CONFIG_MPEG1VIDEO_DECODER
    &ff_mpeg1video_decoder,
#endif
#if CONFIG_MPEG2VIDEO_DECODER
    &ff_mpeg2video_decoder,
#endif
#if CONFIG_ADPCM_ADX_DECODER
    &ff_adpcm_adx_decoder,
#endif
#if CONFIG_WMV3_DECODER
    &ff_wmv3_decoder,
#endif
    NULL};
