#include "suricata-common.h"
#include "suricata.h"

#include "decode.h"

#include "detect.h"

#include "detect-content.h"
#include "detect-uricontent.h"
#include "detect-pcre.h"
#include "detect-isdataat.h"
#include "detect-bytetest.h"
#include "detect-bytejump.h"
#include "detect-http-method.h"
#include "detect-http-cookie.h"

#include "util-spm.h"
#include "util-debug.h"
#include "util-print.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

/** \brief Run the actual payload match functions
 *
 *  \param de_ctx Detection engine context
 *  \param det_ctx Detection engine thread context
 *  \param s Signature to inspect
 *  \param sm SigMatch to inspect
 *  \param f Flow
 *  \param flags app layer flags
 *  \param state App layer state
 *  \param p Packet
 *  \param payload ptr to the payload to inspect
 *  \param payload_len length of the payload
 *
 *  \retval 0 no match
 *  \retval 1 match
 */
static inline int DoInspectPacketPayload(DetectEngineCtx *de_ctx,
        DetectEngineThreadCtx *det_ctx, Signature *s, SigMatch *sm, Flow *f,
        uint8_t flags, void *alstate, Packet *p, uint8_t *payload,
        uint32_t payload_len)
{
    SCEnter();

    if (sm == NULL) {
        SCReturnInt(0);
    }

    switch(sm->type) {
        case DETECT_CONTENT:
        {
            if (payload_len == 0) {
                SCReturnInt(0);
            }

            DetectContentData *cd = NULL;
            cd = (DetectContentData *)sm->ctx;
            SCLogDebug("inspecting content %"PRIu32" payload_len %"PRIu32, cd->id, payload_len);

            /* rule parsers should take care of this */
            BUG_ON(cd->depth != 0 && cd->depth <= cd->offset);

            /* search for our pattern, checking the matches recursively.
             * if we match we look for the next SigMatch as well */
            uint8_t *found = NULL;
            uint32_t offset = 0;
            uint32_t depth = payload_len;
            uint32_t prev_offset = 0; /**< used in recursive searching */

            do {
                if (cd->flags & DETECT_CONTENT_DISTANCE ||
                    cd->flags & DETECT_CONTENT_WITHIN) {
                    SCLogDebug("det_ctx->payload_offset %"PRIu32, det_ctx->payload_offset);

                    offset = det_ctx->payload_offset;
                    depth = payload_len;

                    if (cd->flags & DETECT_CONTENT_DISTANCE) {
                        /** \todo distance can be negative */

                        offset += cd->distance;

                        SCLogDebug("cd->distance %"PRIi32", offset %"PRIu32", depth %"PRIu32,
                            cd->distance, offset, depth);
                    }

                    if (cd->flags & DETECT_CONTENT_WITHIN) {
                        if ((int32_t)depth > (int32_t)(det_ctx->payload_offset + cd->within)) {
                            depth = det_ctx->payload_offset + cd->within;
                        }

                        SCLogDebug("cd->within %"PRIi32", det_ctx->payload_offset %"PRIu32", depth %"PRIu32,
                            cd->within, det_ctx->payload_offset, depth);
                    }

                    if (cd->depth != 0) {
                        if ((cd->depth + det_ctx->payload_offset) < depth) {
                            depth = det_ctx->payload_offset + cd->depth;
                        }

                        SCLogDebug("cd->depth %"PRIu32", depth %"PRIu32, cd->depth, depth);
                    }

                    if (cd->offset > offset) {
                        offset = cd->offset;
                        SCLogDebug("setting offset %"PRIu32, offset);
                    }
                } else { /* implied no relative matches */
                    /* set depth */
                    if (cd->depth != 0) {
                        depth = cd->depth;
                    }

                    /* set offset */
                    offset = cd->offset;
                }

                /* update offset with prev_offset if we're searching for
                 * matches after the first occurence. */
                SCLogDebug("offset %"PRIu32", prev_offset %"PRIu32, prev_offset, depth);
                offset += prev_offset;

                SCLogDebug("offset %"PRIu32", depth %"PRIu32, offset, depth);

                if (depth > payload_len)
                    depth = payload_len;

                /* if offset is bigger than depth we can never match on a pattern.
                 * We can however, "match" on a negated pattern. */
                if (offset > depth || depth == 0) {
                    if (cd->flags & DETECT_CONTENT_NEGATED) {
                        goto match;
                    } else {
                        SCReturnInt(0);
                    }
                }

                uint8_t *spayload = payload + offset;
                uint32_t spayload_len = depth - offset;
                uint32_t match_offset = 0;
                SCLogDebug("spayload_len %"PRIu32, spayload_len);
                BUG_ON(spayload_len > payload_len);

                //PrintRawDataFp(stdout,cd->content,cd->content_len);
                //PrintRawDataFp(stdout,spayload,spayload_len);

                /* do the actual search */
                found = BasicSearch(spayload, spayload_len, cd->content, cd->content_len);

                /* next we evaluate the result in combination with the
                 * negation flag. */
                SCLogDebug("found %p cd negated %s", found, cd->flags & DETECT_CONTENT_NEGATED ? "true" : "false");

                if (found == NULL && !(cd->flags & DETECT_CONTENT_NEGATED)) {
                    SCReturnInt(0);
                } else if (found == NULL && cd->flags & DETECT_CONTENT_NEGATED) {
                    goto match;
                } else if (found != NULL && cd->flags & DETECT_CONTENT_NEGATED) {
#ifdef DEBUG
                    match_offset = (uint32_t)((found - payload) + cd->content_len);
                    SCLogDebug("content %"PRIu32" matched at offset %"PRIu32", but negated so no match", cd->id, match_offset);
#endif
                    SCReturnInt(0);
                } else {
                    match_offset = (uint32_t)((found - payload) + cd->content_len);
                    SCLogDebug("content %"PRIu32" matched at offset %"PRIu32"", cd->id, match_offset);
                    det_ctx->payload_offset = match_offset;

                    if (!(cd->flags & DETECT_CONTENT_RELATIVE_NEXT)) {
                        SCLogDebug("no relative match coming up, so this is a match");
                        goto match;
                    }

                    BUG_ON(sm->next == NULL);
                    SCLogDebug("content %"PRIu32, cd->id);

                    /* see if the next payload keywords match. If not, we will
                     * search for another occurence of this content and see
                     * if the others match then */
                    int r = DoInspectPacketPayload(de_ctx,det_ctx,s,sm->next, f, flags, alstate, p, payload, payload_len);
                    if (r == 1) {
                        SCReturnInt(1);
                    }

                    /* set the previous match offset to the start of this match + 1 */
                    prev_offset += (match_offset - (cd->content_len - 1));
                    SCLogDebug("trying to see if there is another match after prev_offset %"PRIu32, prev_offset);
                }

            } while(1);
        }
        case DETECT_ISDATAAT:
        {
            SCLogDebug("inspecting isdataat");

            DetectIsdataatData *id = (DetectIsdataatData *)sm->ctx;
            if (id->flags & ISDATAAT_RELATIVE) {
                if (det_ctx->payload_offset + id->dataat > payload_len) {
                    SCLogDebug("det_ctx->payload_offset + id->dataat %"PRIu32" > %"PRIu32, det_ctx->payload_offset + id->dataat, payload_len);
                    SCReturnInt(0);
                } else {
                    SCLogDebug("relative isdataat match");
                    goto match;
                }
            } else {
                if (id->dataat < payload_len) {
                    SCLogDebug("absolute isdataat match");
                    goto match;
                } else {
                    SCLogDebug("absolute isdataat mismatch, id->isdataat %"PRIu32", payload_len %"PRIu32"", id->dataat,payload_len);
                    SCReturnInt(0);
                }
            }
        }
        case DETECT_PCRE:
        {
            SCLogDebug("inspecting pcre");

            /** \todo consider ptrs */
            int r = DetectPcreDoMatch(det_ctx, p, s, sm);
            if (r == 1) {
                goto match;
            }

            SCReturnInt(0);
        }
        case DETECT_BYTETEST:
        {
            if (DetectBytetestDoMatch(det_ctx,s,sm,payload,payload_len) != 1) {
                SCReturnInt(0);
            }

            goto match;
        }
        case DETECT_BYTEJUMP:
        {
            if (DetectBytejumpDoMatch(det_ctx,s,sm,payload,payload_len) != 1) {
                SCReturnInt(0);
            }

            goto match;
        }
        default:
        {
            BUG_ON(1);
        }
    }

    SCReturnInt(0);
match:
    if (sm->next != NULL) {
        int r = DoInspectPacketPayload(de_ctx,det_ctx,s,sm->next, f, flags, alstate, p, payload, payload_len);
        SCReturnInt(r);
    } else {
        SCReturnInt(1);
    }
}

/** \brief Do the content inspection for a signature
 *
 *  \param de_ctx Detection engine context
 *  \param det_ctx Detection engine thread context
 *  \param s Signature to inspect
 *  \param sm SigMatch to inspect
 *  \param f Flow
 *  \param flags app layer flags
 *  \param state App layer state
 *  \param p Packet
 *
 *  \retval 0 no match
 *  \retval 1 match
 */
int DetectEngineInspectPacketPayload(DetectEngineCtx *de_ctx,
        DetectEngineThreadCtx *det_ctx, Signature *s, Flow *f, uint8_t flags,
        void *alstate, Packet *p)
{
    SCEnter();
    int r = 0;

    if (s->pmatch == NULL) {
        SCReturnInt(0);
    }

    det_ctx->payload_offset = 0;

    r = DoInspectPacketPayload(de_ctx, det_ctx, s, s->pmatch, f, flags, alstate, p, p->payload, p->payload_len);
    if (r == 1) {
        SCReturnInt(1);
    }

    SCReturnInt(0);
}

#ifdef UNITTESTS

static int PayloadTestSig01 (void) {
    uint8_t *buf = (uint8_t *)
                    "abcabcd";
    uint16_t buflen = strlen((char *)buf);
    Packet *p = UTHBuildPacket( buf, buflen, IPPROTO_TCP);
    int result = 0;

    char sig[] = "alert tcp any any -> any any (content:\"abc\"; content:\"d\"; distance:0; within:1; sid:1;)";
    if (UTHPacketMatchSigMpm(p, sig, MPM_B2G) == 0) {
        result = 0;
        goto end;
    }

    result = 1;
end:
    if (p != NULL)
        UTHFreePacket(p);
    return result;
}

#endif /* UNITTESTS */

void PayloadRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("PayloadTestSig01", PayloadTestSig01, 1);
#endif /* UNITTESTS */
}

