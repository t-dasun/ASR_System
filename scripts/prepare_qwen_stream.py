"""Derive a resumable CPU API from the pinned native streaming loop.

Only the generated build copy is extended. One step executes one native audio
iteration. Encoder-window caches, token history and decoder prefill/KV are owned
by a call; model weight allocations and mmaps are borrowed from the worker.
"""
import re


def extend(header, source):
    declarations = '''
/* Platform extension: CPU-only, serialized use of shared immutable weights.
 * The parent model must outlive every stream. No whole-audio final refinement.
 * step: 0 needs audio, 1 progressed, 2 final, negative failed/interrupted. */
typedef struct qwen_resumable_state qwen_resumable_state;
qwen_resumable_state *qwen_resumable_create(qwen_ctx_t *model, const char *language,
    int step_ms, int max_tokens, int unfixed_chunks);
int qwen_resumable_step(qwen_resumable_state *s, const float *audio, int count, int eof);
const char *qwen_resumable_text(const qwen_resumable_state *s);
int64_t qwen_resumable_cursor(const qwen_resumable_state *s);
int qwen_resumable_steps(const qwen_resumable_state *s);
int64_t qwen_resumable_reused_prefill(const qwen_resumable_state *s);
void qwen_resumable_destroy(qwen_resumable_state *s);
'''
    anchor = '/* Transcribe a WAV file, returns allocated string (caller must free) */'
    assert header.count(anchor) == 1
    header = header.replace(anchor, declarations + '\n' + anchor)

    # Persistent variables in the upstream stream_impl invocation become fields.
    groups = {
        'qwen_tokenizer_t *': ['tokenizer'],
        'int *': ['raw_tokens', 'stable_text_tokens', 'emitted_text_tokens'],
        'int ': ['n_raw_tokens', 'raw_tokens_cap', 'n_stable_text_tokens', 'stable_text_cap',
                 'n_emitted_text_tokens', 'emitted_text_cap', 'stagnant_chunks', 'chunk_idx',
                 'n_enc_cache', 'enc_cache_start', 'enc_cache_cap', 'enc_cached_seq_total',
                 'prev_prefill_len', 'prev_prefill_cap', 'prefill_total_tokens', 'prefill_reused_tokens'],
        'size_t ': ['result_cap', 'result_len'],
        'char *': ['result'],
        'float *': ['tmp_embed', 'prev_prefill_embeds'],
        'int64_t ': ['audio_cursor', 'next_window_start'],
        'stream_enc_window_t *': ['enc_cache'],
    }
    fields = [name for names in groups.values() for name in names]
    definition = 'struct qwen_resumable_state {\n    qwen_ctx_t *ctx, *model;\n    int finished;\n'
    definition += ''.join(f'    {kind}{name};\n' for kind,names in groups.items() for name in names)
    definition += '};\n'
    free_block = source.split('    /* KV cache */', 1)[1].split('    /* Close safetensors */', 1)[0]
    private = re.findall(r'free\(ctx->(\w+)\)', free_block)
    assert len(private) == len(set(private)) and len(private) >= 30
    destroy = '''void qwen_resumable_destroy(qwen_resumable_state *s) {
    if (!s) return;
    if (s->tokenizer) qwen_tokenizer_free(s->tokenizer);
    for (int i = s->enc_cache_start; i < s->n_enc_cache; ++i) free(s->enc_cache[i].enc_output);
'''
    destroy += ''.join(f'    free(s->{n});\n' for n in ('enc_cache','raw_tokens','stable_text_tokens',
        'emitted_text_tokens','result','tmp_embed','prev_prefill_embeds'))
    destroy += '    if (s->ctx) {\n'
    destroy += ''.join(f'        free(s->ctx->{n});\n' for n in private)
    destroy += '        free(s->ctx);\n    }\n    free(s);\n}\n'
    create = '''qwen_resumable_state *qwen_resumable_create(qwen_ctx_t *model, const char *language,
    int step_ms, int max_tokens, int unfixed_chunks) {
    if (!model || model->rocm || model->cuda || step_ms < 1000 || step_ms > 8000 ||
        max_tokens < 1 || max_tokens > 256 || unfixed_chunks < 0 || unfixed_chunks > 4) return NULL;
    qwen_resumable_state *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->model = model;
    s->ctx = malloc(sizeof(*s->ctx));
    if (!s->ctx) { free(s); return NULL; }
    *s->ctx = *model; /* Borrow weights; never qwen_free this shallow context. */
'''
    create += ''.join(f'    s->ctx->{n} = NULL;\n' for n in private)
    create += '''    s->ctx->kv_cache_len = s->ctx->kv_cache_max = 0;
    s->ctx->pref_seq_cap = s->ctx->rope_cache_cap = s->ctx->rope_inv_freq_half = 0;
    s->ctx->n_prompt_tokens = s->ctx->n_force_prompt_tokens = s->ctx->prompt_tokens_ready = 0;
    s->ctx->token_cb = NULL; s->ctx->token_cb_userdata = NULL;
    s->ctx->stream_chunk_sec = step_ms / 1000.0f;
    s->ctx->stream_max_new_tokens = max_tokens;
    s->ctx->stream_unfixed_chunks = unfixed_chunks;
    s->ctx->past_text_conditioning = 1;
    s->ctx->perf_total_ms = s->ctx->perf_encode_ms = s->ctx->perf_decode_ms = 0;
    s->ctx->perf_audio_ms = 0; s->ctx->perf_text_tokens = 0;
    if (model->prompt) s->ctx->prompt = strdup(model->prompt);
    if ((model->prompt && !s->ctx->prompt) || qwen_set_force_language(s->ctx, language) != 0)
        goto failure;
    char vocab_path[1024];
    snprintf(vocab_path, sizeof(vocab_path), "%s/vocab.json", model->model_dir);
    s->tokenizer = qwen_tokenizer_load(vocab_path);
    if (!s->tokenizer || prepare_prompt_tokens(s->ctx, s->tokenizer) != 0) goto failure;
    s->raw_tokens_cap = s->stable_text_cap = s->emitted_text_cap = 8192;
    s->raw_tokens = malloc(8192 * sizeof(int));
    s->stable_text_tokens = malloc(8192 * sizeof(int));
    s->emitted_text_tokens = malloc(8192 * sizeof(int));
    s->result_cap = 4096;
    s->result = calloc(1, s->result_cap);
    s->tmp_embed = malloc(model->config.dec_hidden * sizeof(float));
    if (!s->raw_tokens || !s->stable_text_tokens || !s->emitted_text_tokens || !s->result || !s->tmp_embed)
        goto failure;
    return s;
failure:
    qwen_resumable_destroy(s);
    return NULL;
}
'''
    original = source.split('static char *stream_impl(', 1)[1].split('\nchar *qwen_transcribe_stream(', 1)[0]
    body = original.split('    while (audio_cursor < audio_n_samples || (live && !live_eof)) {', 1)[1]
    body = body.split('\n    free(tmp_embed);',1)[0]
    assert body.endswith('\n    }\n')
    body = body[:-len('\n    }\n')]
    # Live input is supplied by the scheduler: never block or consume future audio.
    start = body.index('        /* Live mode: wait until we have enough data for the next chunk. */')
    end = body.index('        double chunk_t0 = get_time_ms();', start)
    body = body[:start] + body[end:]
    body = body.replace('int is_final = live ? (live_eof && audio_cursor >= audio_n_samples)\n                            : (audio_cursor >= audio_n_samples);',
                        'int is_final = eof && audio_cursor >= audio_n_samples;')
    assert body.count('        while (n_generated < max_new_tokens) {') == 1
    body = body.replace('        while (n_generated < max_new_tokens) {', '''        while (n_generated < max_new_tokens) {
            if (ctx->offline_decode_guard && !ctx->offline_decode_guard(ctx->offline_decode_guard_userdata)) {
                ctx->offline_decode_aborted = 1;
                break;
            }''')
    # Native iteration uses these loop constants; its bounds/recovery policies remain intact.
    constants = original.split('    #define QWEN_STREAM_MAX_ENC_WINDOWS',1)[1].split('\n\n    if (qwen_verbose',1)[0]
    constants = '    #define QWEN_STREAM_MAX_ENC_WINDOWS' + constants
    aliases = ''.join(f'#define {name} (s->{name})\n' for name in fields)
    unalias = ''.join(f'#undef {name}\n' for name in fields)
    step = '''int qwen_resumable_step(qwen_resumable_state *s, const float *samples, int n_samples, int eof) {
    if (!s || n_samples < 0 || (!samples && n_samples) || n_samples < s->audio_cursor || s->finished) return -1;
    qwen_ctx_t *ctx = s->ctx;
    const qwen_config_t *cfg = &ctx->config;
    int dim = cfg->dec_hidden;
    int chunk_samples = (int)(ctx->stream_chunk_sec * QWEN_SAMPLE_RATE);
    if (!eof && n_samples < s->audio_cursor + chunk_samples) return 0;
    ctx->offline_decode_guard = s->model->offline_decode_guard;
    ctx->offline_decode_guard_userdata = s->model->offline_decode_guard_userdata;
    ctx->offline_decode_aborted = 0;
    int rollback = ctx->stream_rollback, unfixed_chunks = ctx->stream_unfixed_chunks;
    int max_new_tokens = ctx->stream_max_new_tokens;
    const float *audio_samples = samples;
    int64_t audio_n_samples = n_samples;
    qwen_live_audio_t *live = NULL;
    int live_eof = eof, use_enc_cache = 1;
    int64_t local_base_sample = 0, local_n_samples = n_samples;
    float *local_samples = NULL;
    int enc_window_frames = ctx->config.enc_n_window_infer;
    if (enc_window_frames < 100) enc_window_frames = 100;
    if (enc_window_frames > 800) enc_window_frames = 800;
    int enc_window_samples = enc_window_frames * QWEN_HOP_LENGTH;
    ctx->perf_audio_ms = 1000.0 * n_samples / QWEN_SAMPLE_RATE;
'''
    step += constants + '\n' + aliases
    step += '    int iterations = 0;\n    while ((audio_cursor < audio_n_samples || eof) && iterations++ == 0) {\n'
    step += body + '\n    }\n'
    step += unalias
    step += '''    ctx->offline_decode_guard = NULL;
    ctx->offline_decode_guard_userdata = NULL;
    if (ctx->offline_decode_aborted) return -2;
    s->finished = eof && s->audio_cursor >= n_samples;
    if (s->finished) {
        size_t len = strlen(s->result);
        while (len && isspace((unsigned char)s->result[len-1])) s->result[--len] = 0;
        char *start = s->result;
        while (*start && isspace((unsigned char)*start)) start++;
        if (start != s->result) memmove(s->result, start, strlen(start) + 1);
    }
    return s->finished ? 2 : 1;
}
const char *qwen_resumable_text(const qwen_resumable_state *s) { return s ? s->result : ""; }
int64_t qwen_resumable_cursor(const qwen_resumable_state *s) { return s ? s->audio_cursor : 0; }
int qwen_resumable_steps(const qwen_resumable_state *s) { return s ? s->chunk_idx : 0; }
int64_t qwen_resumable_reused_prefill(const qwen_resumable_state *s) { return s ? s->prefill_reused_tokens : 0; }
'''
    insertion = '\nchar *qwen_transcribe_stream(qwen_ctx_t *ctx,'
    assert source.count(insertion) == 1
    source = source.replace(insertion, '\n' + definition + destroy + create + step + insertion)
    return header, source
