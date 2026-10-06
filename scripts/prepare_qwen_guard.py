#!/usr/bin/env python3
"""Derive CPU build sources with an optional offline decode guard.
The pinned vendor checkout stays pristine. Unset guards preserve upstream behavior.
"""
import argparse
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('source',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=True)
for source in [*args.source.glob('*.h'),*args.source.glob('*.c')]:
    text=source.read_text()
    if source.name=='qwen_asr.h':
        anchor='    void *token_cb_userdata;'
        assert text.count(anchor)==1
        text=text.replace(anchor,anchor+'''

    /* ASR platform extension: optional cooperative offline decode guard.
     * Return nonzero to continue. Checked between generated tokens; encoder
     * and individual kernel calls remain blocking. Abort returns partial text,
     * which the caller must reject when offline_decode_aborted is set. */
    int (*offline_decode_guard)(void *userdata);
    void *offline_decode_guard_userdata;
    int offline_decode_aborted;''')
    if source.name=='qwen_asr.c':
        anchor='char *qwen_transcribe_audio(qwen_ctx_t *ctx, const float *samples, int n_samples) {'
        assert text.count(anchor)==1
        text=text.replace(anchor,anchor+'\n    ctx->offline_decode_aborted = 0;')
        anchor='    while (n_generated < max_tokens) {'
        assert text.count(anchor)==1
        text=text.replace(anchor,anchor+'''
        if (ctx->offline_decode_guard &&
            !ctx->offline_decode_guard(ctx->offline_decode_guard_userdata)) {
            ctx->offline_decode_aborted = 1;
            break;
        }''')
    destination=args.output/source.name
    if not destination.exists() or destination.read_text()!=text:
        destination.write_text(text)
