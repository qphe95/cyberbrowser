#include "audio_extract.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"

#define LOG_TAG "audio_extract"
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef BE_PLATFORM_ANDROID
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

static void set_err(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

static bool is_supported_audio_mime(const char *mime) {
    return mime && strcmp(mime, "audio/mp4a-latm") == 0;
}

static bool remux_aac_to_fd(AMediaExtractor *extractor, AMediaFormat *audioFormat,
                            int audioTrack, int outFd,
                            char *err, size_t errLen) {
    if (AMediaExtractor_selectTrack(extractor, audioTrack) != AMEDIA_OK) {
        set_err(err, errLen, "Select audio track failed");
        return false;
    }

    AMediaMuxer *muxer = AMediaMuxer_new(outFd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer) {
        set_err(err, errLen, "Muxer create failed");
        return false;
    }
    int outTrack = AMediaMuxer_addTrack(muxer, audioFormat);
    if (outTrack < 0 || AMediaMuxer_start(muxer) != AMEDIA_OK) {
        AMediaMuxer_delete(muxer);
        set_err(err, errLen, "Muxer start failed");
        return false;
    }

    int32_t maxInputSize = 0;
    if (!AMediaFormat_getInt32(audioFormat, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                               &maxInputSize)) {
        maxInputSize = 512 * 1024;
    }
    uint8_t *buffer = (uint8_t *)malloc((size_t)maxInputSize);
    if (!buffer) {
        AMediaMuxer_stop(muxer);
        AMediaMuxer_delete(muxer);
        set_err(err, errLen, "Alloc buffer failed");
        return false;
    }

    while (true) {
        ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, buffer, maxInputSize);
        if (sampleSize < 0) {
            break;
        }
        AMediaCodecBufferInfo info = {0};
        info.offset = 0;
        info.size = (int32_t)sampleSize;
        info.presentationTimeUs = AMediaExtractor_getSampleTime(extractor);
        info.flags = AMediaExtractor_getSampleFlags(extractor);
        if (AMediaMuxer_writeSampleData(muxer, outTrack, buffer, &info) != AMEDIA_OK) {
            free(buffer);
            AMediaMuxer_stop(muxer);
            AMediaMuxer_delete(muxer);
            set_err(err, errLen, "Muxer write failed");
            return false;
        }
        AMediaExtractor_advance(extractor);
    }

    free(buffer);
    AMediaMuxer_stop(muxer);
    AMediaMuxer_delete(muxer);
    return true;
}

static bool transcode_to_aac(const char *inputPath, int outFd,
                             char *err, size_t errLen) {
    LOGI("transcode_to_aac: inputPath=%s", inputPath);
    int inFd = open(inputPath, O_RDONLY);
    if (inFd < 0) {
        set_err(err, errLen, "Failed to open input file");
        LOGE("Failed to open input file: %s", inputPath);
        return false;
    }
    
    AMediaExtractor *extractor = AMediaExtractor_new();
    if (!extractor) {
        close(inFd);
        set_err(err, errLen, "Extractor create failed");
        return false;
    }
    media_status_t status = AMediaExtractor_setDataSourceFd(extractor, inFd, 0, 0);
    close(inFd);
    if (status != AMEDIA_OK) {
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Set data source failed");
        LOGE("AMediaExtractor_setDataSourceFd failed with status: %d", status);
        return false;
    }

    ssize_t trackCount = AMediaExtractor_getTrackCount(extractor);
    int audioTrack = -1;
    AMediaFormat *audioFormat = NULL;
    const char *mime = NULL;
    for (ssize_t i = 0; i < trackCount; ++i) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, i);
        const char *trackMime = NULL;
        if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &trackMime)) {
            if (strncmp(trackMime, "audio/", 6) == 0) {
                audioTrack = (int)i;
                audioFormat = format;
                mime = trackMime;
                break;
            }
        }
        AMediaFormat_delete(format);
    }
    if (audioTrack < 0 || !audioFormat || !mime) {
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "No audio track found");
        return false;
    }
    if (AMediaExtractor_selectTrack(extractor, audioTrack) != AMEDIA_OK) {
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Select audio track failed");
        return false;
    }

    int32_t sampleRate = 0;
    int32_t channels = 0;
    AMediaFormat_getInt32(audioFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sampleRate);
    AMediaFormat_getInt32(audioFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
    if (sampleRate <= 0 || channels <= 0) {
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Invalid audio format");
        return false;
    }

    AMediaCodec *decoder = AMediaCodec_createDecoderByType(mime);
    if (!decoder) {
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Decoder create failed");
        return false;
    }
    if (AMediaCodec_configure(decoder, audioFormat, NULL, NULL, 0) != AMEDIA_OK ||
        AMediaCodec_start(decoder) != AMEDIA_OK) {
        AMediaCodec_delete(decoder);
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Decoder start failed");
        return false;
    }

    AMediaFormat *encFormat = AMediaFormat_new();
    AMediaFormat_setString(encFormat, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(encFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, sampleRate);
    AMediaFormat_setInt32(encFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
    AMediaFormat_setInt32(encFormat, AMEDIAFORMAT_KEY_BIT_RATE, 128000);
    AMediaFormat_setInt32(encFormat, AMEDIAFORMAT_KEY_AAC_PROFILE, 2);

    AMediaCodec *encoder = AMediaCodec_createEncoderByType("audio/mp4a-latm");
    if (!encoder) {
        AMediaFormat_delete(encFormat);
        AMediaCodec_stop(decoder);
        AMediaCodec_delete(decoder);
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Encoder create failed");
        return false;
    }
    if (AMediaCodec_configure(encoder, encFormat, NULL, NULL,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        AMediaCodec_start(encoder) != AMEDIA_OK) {
        AMediaCodec_delete(encoder);
        AMediaFormat_delete(encFormat);
        AMediaCodec_stop(decoder);
        AMediaCodec_delete(decoder);
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Encoder start failed");
        return false;
    }

    AMediaMuxer *muxer = AMediaMuxer_new(outFd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if (!muxer) {
        AMediaCodec_stop(encoder);
        AMediaCodec_delete(encoder);
        AMediaFormat_delete(encFormat);
        AMediaCodec_stop(decoder);
        AMediaCodec_delete(decoder);
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Muxer create failed");
        return false;
    }

    bool inputEos = false;
    bool decoderEos = false;
    bool encoderEos = false;
    bool muxerStarted = false;
    int outTrack = -1;

    while (!encoderEos) {
        if (!inputEos) {
            ssize_t inIndex = AMediaCodec_dequeueInputBuffer(decoder, 10000);
            if (inIndex >= 0) {
                size_t inSize = 0;
                uint8_t *inBuf = AMediaCodec_getInputBuffer(decoder, (size_t)inIndex, &inSize);
                ssize_t sampleSize = AMediaExtractor_readSampleData(extractor, inBuf, inSize);
                if (sampleSize < 0) {
                    AMediaCodec_queueInputBuffer(decoder, (size_t)inIndex, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inputEos = true;
                } else {
                    int64_t pts = AMediaExtractor_getSampleTime(extractor);
                    AMediaCodec_queueInputBuffer(decoder, (size_t)inIndex, 0,
                                                 (size_t)sampleSize, pts, 0);
                    AMediaExtractor_advance(extractor);
                }
            }
        }

        AMediaCodecBufferInfo decInfo;
        ssize_t decIndex = AMediaCodec_dequeueOutputBuffer(decoder, &decInfo, 10000);
        if (decIndex >= 0) {
            if (decInfo.size > 0) {
                size_t pcmSize = 0;
                uint8_t *pcm = AMediaCodec_getOutputBuffer(decoder, (size_t)decIndex, &pcmSize);
                ssize_t encIn = AMediaCodec_dequeueInputBuffer(encoder, 10000);
                if (encIn >= 0) {
                    size_t encInSize = 0;
                    uint8_t *encBuf = AMediaCodec_getInputBuffer(encoder, (size_t)encIn, &encInSize);
                    size_t copySize = (size_t)decInfo.size;
                    if (copySize > encInSize) {
                        copySize = encInSize;
                    }
                    memcpy(encBuf, pcm + decInfo.offset, copySize);
                    AMediaCodec_queueInputBuffer(encoder, (size_t)encIn, 0, copySize,
                                                 decInfo.presentationTimeUs, 0);
                }
            }
            if (decInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                decoderEos = true;
                ssize_t encIn = AMediaCodec_dequeueInputBuffer(encoder, 10000);
                if (encIn >= 0) {
                    AMediaCodec_queueInputBuffer(encoder, (size_t)encIn, 0, 0,
                                                 decInfo.presentationTimeUs,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                }
            }
            AMediaCodec_releaseOutputBuffer(decoder, (size_t)decIndex, false);
        }

        AMediaCodecBufferInfo encInfo;
        ssize_t encIndex = AMediaCodec_dequeueOutputBuffer(encoder, &encInfo, 10000);
        if (encIndex == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *outFormat = AMediaCodec_getOutputFormat(encoder);
            outTrack = AMediaMuxer_addTrack(muxer, outFormat);
            AMediaFormat_delete(outFormat);
            if (outTrack < 0 || AMediaMuxer_start(muxer) != AMEDIA_OK) {
                set_err(err, errLen, "Muxer start failed");
                break;
            }
            muxerStarted = true;
        } else if (encIndex >= 0) {
            if (encInfo.size > 0 && muxerStarted) {
                size_t outSize = 0;
                uint8_t *outBuf = AMediaCodec_getOutputBuffer(encoder, (size_t)encIndex, &outSize);
                AMediaMuxer_writeSampleData(muxer, outTrack, outBuf, &encInfo);
            }
            if (encInfo.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                encoderEos = true;
            }
            AMediaCodec_releaseOutputBuffer(encoder, (size_t)encIndex, false);
        }
    }

    if (muxerStarted) {
        AMediaMuxer_stop(muxer);
    }
    AMediaMuxer_delete(muxer);
    AMediaCodec_stop(encoder);
    AMediaCodec_delete(encoder);
    AMediaFormat_delete(encFormat);
    AMediaCodec_stop(decoder);
    AMediaCodec_delete(decoder);
    AMediaFormat_delete(audioFormat);
    AMediaExtractor_delete(extractor);
    return encoderEos;
}

bool audio_extract_to_fd(const char *inputPath, int outFd,
                         char *err, size_t errLen) {
    LOGI("audio_extract_to_fd: inputPath=%s", inputPath);
    int inFd = open(inputPath, O_RDONLY);
    if (inFd < 0) {
        set_err(err, errLen, "Failed to open input file");
        LOGE("Failed to open input file: %s", inputPath);
        return false;
    }
    
    // Check if file is HTML (common mistake - downloaded HTML instead of media)
    char header[64] = {0};
    ssize_t headerRead = read(inFd, header, sizeof(header) - 1);
    if (headerRead > 0) {
        header[headerRead] = '\0';
        // Check for HTML markers
        if (strstr(header, "<html") || strstr(header, "<!DOCTYPE") ||
            strstr(header, "<HTML") || strstr(header, "<!doctype")) {
            close(inFd);
            set_err(err, errLen, "Downloaded file is HTML, not media. URL extraction may have failed.");
            LOGE("File appears to be HTML, not a media file");
            return false;
        }
    }
    lseek(inFd, 0, SEEK_SET); // Reset file pointer
    
    AMediaExtractor *extractor = AMediaExtractor_new();
    if (!extractor) {
        close(inFd);
        set_err(err, errLen, "Extractor create failed");
        return false;
    }
    
    // Get file size for setDataSourceFd
    struct stat st;
    off_t fileSize = 0;
    if (fstat(inFd, &st) == 0) {
        fileSize = st.st_size;
    }
    
    media_status_t status = AMediaExtractor_setDataSourceFd(extractor, inFd, 0, fileSize);
    close(inFd);
    if (status != AMEDIA_OK) {
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "Set data source failed");
        LOGE("AMediaExtractor_setDataSourceFd failed with status: %d", status);
        return false;
    }

    ssize_t trackCount = AMediaExtractor_getTrackCount(extractor);
    int audioTrack = -1;
    AMediaFormat *audioFormat = NULL;
    const char *mime = NULL;
    for (ssize_t i = 0; i < trackCount; ++i) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, i);
        const char *trackMime = NULL;
        if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &trackMime)) {
            if (strncmp(trackMime, "audio/", 6) == 0) {
                audioTrack = (int)i;
                audioFormat = format;
                mime = trackMime;
                break;
            }
        }
        AMediaFormat_delete(format);
    }
    if (audioTrack < 0 || !audioFormat) {
        AMediaExtractor_delete(extractor);
        set_err(err, errLen, "No audio track found");
        return false;
    }
    if (is_supported_audio_mime(mime)) {
        bool ok = remux_aac_to_fd(extractor, audioFormat, audioTrack, outFd, err, errLen);
        AMediaFormat_delete(audioFormat);
        AMediaExtractor_delete(extractor);
        return ok;
    }

    AMediaFormat_delete(audioFormat);
    AMediaExtractor_delete(extractor);
    return transcode_to_aac(inputPath, outFd, err, errLen);
}
