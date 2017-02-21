/*************************************************************************/
/*                                                                       */
/*                  Language Technologies Institute                      */
/*                     Carnegie Mellon University                        */
/*                        Copyright (c) 2000                             */
/*                        All Rights Reserved.                           */
/*                                                                       */
/*  Permission is hereby granted, free of charge, to use and distribute  */
/*  this software and its documentation without restriction, including   */
/*  without limitation the rights to use, copy, modify, merge, publish,  */
/*  distribute, sublicense, and/or sell copies of this work, and to      */
/*  permit persons to whom this work is furnished to do so, subject to   */
/*  the following conditions:                                            */
/*   1. The code must retain the above copyright notice, this list of    */
/*      conditions and the following disclaimer.                         */
/*   2. Any modifications must be clearly marked as such.                */
/*   3. Original authors' names are not deleted.                         */
/*   4. The authors' names are not used to endorse or promote products   */
/*      derived from this software without specific prior written        */
/*      permission.                                                      */
/*                                                                       */
/*  CARNEGIE MELLON UNIVERSITY AND THE CONTRIBUTORS TO THIS WORK         */
/*  DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING      */
/*  ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT   */
/*  SHALL CARNEGIE MELLON UNIVERSITY NOR THE CONTRIBUTORS BE LIABLE      */
/*  FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES    */
/*  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN   */
/*  AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,          */
/*  ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF       */
/*  THIS SOFTWARE.                                                       */
/*                                                                       */
/*************************************************************************/
/*             Author:  Alan W Black (awb@cs.cmu.edu)                    */
/*               Date:  October 2000                                     */
/*************************************************************************/
/*                                                                       */
/*  Access to audio devices                                   ,          */
/*                                                                       */
/*************************************************************************/
#include "cst_string.h"
#include "cst_wave.h"
#include "cst_audio.h"
#include "native_audio.h"
#include <errno.h>

volatile int shutdown_request = 0;
cst_audiodev *ad_playing = NULL;

void mimic_audio_shutdown(int signum)
{
    (void) signum;
    shutdown_request = 1;
    if (ad_playing != NULL)
        mimic_audio_drain(ad_playing);
}

int mimic_audio_bps(cst_audiofmt fmt)
{
    switch (fmt)
    {
    case CST_AUDIO_LINEAR16:
        return 2;
    case CST_AUDIO_LINEAR8:
    case CST_AUDIO_MULAW:
        return 1;
    }
    return 0;
}

cst_audiodev *mimic_audio_open(int sps, int channels, cst_audiofmt fmt)
{
    cst_audiodev *ad;
    int up, down;

    ad = AUDIO_OPEN_NATIVE(sps, channels, fmt);
    if (ad == NULL)
        return NULL;

    down = sps / 1000;
    up = ad->real_sps / 1000;

    if (up != down)
        ad->rateconv = new_rateconv(up, down, channels);

    return ad;
}

int mimic_audio_close(cst_audiodev *ad)
{
    if (ad->rateconv)
        delete_rateconv(ad->rateconv);

    return AUDIO_CLOSE_NATIVE(ad);
}

int mimic_audio_write(cst_audiodev *ad, void *buff, int num_bytes)
{
    void *abuf = buff, *nbuf = NULL;
    int rv, i, real_num_bytes = num_bytes;

    if (ad->rateconv)
    {
        short *in, *out;
        int insize, outsize, n;

        insize = real_num_bytes / 2;
        in = (short *) buff;

        outsize = ad->rateconv->outsize;
        nbuf = out = cst_alloc(short, outsize);
        real_num_bytes = outsize * 2;

        while ((n = cst_rateconv_in(ad->rateconv, in, insize)) > 0)
        {
            in += n;
            insize -= n;
            while ((n = cst_rateconv_out(ad->rateconv, out, outsize)) > 0)
            {
                out += n;
                outsize -= n;
            }
        }
        real_num_bytes -= outsize * 2;
        if (abuf != buff)
            cst_free(abuf);
        abuf = nbuf;
    }
    if (ad->real_channels != ad->channels)
    {
        nbuf = cst_alloc(char,
                         real_num_bytes * ad->real_channels / ad->channels);

        /* Yeah, we only do mono->stereo for now */
        if (ad->real_channels != 2 || ad->channels != 1)
        {
            cst_errmsg
                ("mimic_audio_write: unsupported channel mapping requested (%d => %d).\n",
                 ad->channels, ad->real_channels);
        }

        if (mimic_audio_bps(ad->fmt) == 2)
        {
            for (i = 0; i < real_num_bytes / 2; ++i)
            {
                ((short *) nbuf)[i * 2] = ((short *) abuf)[i];
                ((short *) nbuf)[i * 2 + 1] = ((short *) abuf)[i];
            }
        }
        else if (mimic_audio_bps(ad->fmt) == 1)
        {
            for (i = 0; i < real_num_bytes / 2; ++i)
            {
                ((unsigned char *) nbuf)[i * 2] = ((unsigned char *) abuf)[i];
                ((unsigned char *) nbuf)[i * 2 + 1] =
                    ((unsigned char *) abuf)[i];
            }
        }
        else
        {
            cst_errmsg("mimic_audio_write: unknown format %d\n", ad->fmt);
            cst_free(nbuf);
            if (abuf != buff)
                cst_free(abuf);
            cst_error();
        }

        if (abuf != buff)
            cst_free(abuf);
        abuf = nbuf;
        real_num_bytes = real_num_bytes * ad->real_channels / ad->channels;
    }
    if (ad->real_fmt != ad->fmt)
    {
        if (ad->real_fmt == CST_AUDIO_LINEAR16 && ad->fmt == CST_AUDIO_MULAW)
        {
            nbuf = cst_alloc(char, real_num_bytes * 2);
            for (i = 0; i < real_num_bytes; ++i)
                ((short *) nbuf)[i] =
                    cst_ulaw_to_short(((unsigned char *) abuf)[i]);
            real_num_bytes *= 2;
        }
        else if (ad->real_fmt == CST_AUDIO_MULAW
                 && ad->fmt == CST_AUDIO_LINEAR16)
        {
            nbuf = cst_alloc(char, real_num_bytes / 2);
            for (i = 0; i < real_num_bytes / 2; ++i)
                ((unsigned char *) nbuf)[i] =
                    cst_short_to_ulaw(((short *) abuf)[i]);
            real_num_bytes /= 2;
        }
        else if (ad->real_fmt == CST_AUDIO_LINEAR8
                 && ad->fmt == CST_AUDIO_LINEAR16)
        {
            nbuf = cst_alloc(char, real_num_bytes / 2);
            for (i = 0; i < real_num_bytes / 2; ++i)
                ((unsigned char *) nbuf)[i] =
                    (((short *) abuf)[i] >> 8) + 128;
            real_num_bytes /= 2;
        }
        else
        {
            cst_errmsg
                ("mimic_audio_write: unknown format conversion (%d => %d) requested.\n",
                 ad->fmt, ad->real_fmt);
            if ((abuf != nbuf) && (abuf != buff))
                cst_free(abuf);
            cst_free(nbuf);
            cst_error();
        }
        if (abuf != buff)
            cst_free(abuf);
        abuf = nbuf;
    }
    if (ad->byteswap && mimic_audio_bps(ad->real_fmt) == 2)
        swap_bytes_short((short *) abuf, real_num_bytes / 2);

    if (real_num_bytes)
        rv = AUDIO_WRITE_NATIVE(ad, abuf, real_num_bytes);
    else
        rv = 0;

    if (abuf != buff)
        cst_free(abuf);

    /* Callers expect to get the same num_bytes back as they passed
       in.  Funny, that ... */
    return (rv == real_num_bytes) ? num_bytes : 0;
}

int mimic_audio_init()
{
    return AUDIO_INIT_NATIVE();
}

int mimic_audio_exit()
{
    return AUDIO_EXIT_NATIVE();
}

int mimic_audio_drain(cst_audiodev *ad)
{
    return AUDIO_DRAIN_NATIVE(ad);
}

int mimic_audio_flush(cst_audiodev *ad)
{
    return AUDIO_FLUSH_NATIVE(ad);
}

static void mimic_audio_write_buffer(cst_audiodev *ad, void *buff,
                               int num_samples, int num_channels)
{
    int n, r, i;
    int num_shorts = num_samples * num_channels;
    int bytes_per_short = mimic_audio_bps(ad->fmt);
    int num_bytes = num_shorts * bytes_per_short;
    if (CST_AUDIOBUFFSIZE <= 0)
    {
        r = mimic_audio_write(ad, buff, num_bytes);
        if (r <= 0)
        {
            cst_errmsg("failed to write %d samples\n", num_shorts);
        }
    }
    else
    {
        for (i = 0; i < num_shorts; i += r / 2)
        {
            if (num_shorts > i + CST_AUDIOBUFFSIZE)
                n = CST_AUDIOBUFFSIZE;
            else
                n = num_shorts - i;
            /* buff casted to char because a 1 unit buff increment must
             * be one byte */
            r = mimic_audio_write(ad, ((char*)buff) + i * bytes_per_short, n * 2);
            if (r <= 0)
            {
                cst_errmsg("failed to write %d samples\n", n);
                break;
            }
            if (shutdown_request)
                break;
        }
    }
    return;
}


int mimic_play_wave(cst_wave *w)
{
    if (!w)
        return CST_ERROR_FORMAT;

    if ((ad_playing = mimic_audio_open(w->sample_rate, w->num_channels,
                                 /* FIXME: should be able to determine this somehow */
                                 CST_AUDIO_LINEAR16)) == NULL)
        return CST_ERROR_FORMAT;
    mimic_audio_write_buffer(ad_playing, w->samples, w->num_samples,
                       w->num_channels);
    mimic_audio_close(ad_playing);
    if (shutdown_request)
    {
        cst_errmsg("Shutdown requested!\n");
        return EINTR;
    }
    else
        return CST_OK_FORMAT;
}

int mimic_play_wave_sync(cst_wave *w, cst_relation *rel,
                   int (*call_back) (cst_item *))
{
    int q, i, n, r;
    cst_audiodev *ad;
    float r_pos;
    cst_item *item;

    if (CST_AUDIOBUFFSIZE <= 0)
    {
        cst_errmsg("mimic_play_wave_sync not compatible with PortAudio\n");
        return -1;
    }

    if (!w)
        return CST_ERROR_FORMAT;

    if ((ad = mimic_audio_open(w->sample_rate, w->num_channels,
                         CST_AUDIO_LINEAR16)) == NULL)
        return CST_ERROR_FORMAT;

    q = 0;
    item = relation_head(rel);
    r_pos = w->sample_rate * 0;
    for (i = 0; i < w->num_samples; i += r / 2)
    {
        if (i >= r_pos)
        {
            mimic_audio_flush(ad);
            if ((*call_back) (item) != CST_OK_FORMAT)
                break;
            item = item_next(item);
            if (item)
                r_pos = w->sample_rate * val_float(ffeature(item, "p.end"));
            else
                r_pos = w->num_samples;
        }
        if (w->num_samples > i + CST_AUDIOBUFFSIZE)
            n = CST_AUDIOBUFFSIZE;
        else
            n = w->num_samples - i;

        r = mimic_audio_write(ad, &w->samples[i], n * 2);
        q += r;
        if (r <= 0)
            cst_errmsg("failed to write %d samples\n", n);
    }

    mimic_audio_close(ad);

    return CST_OK_FORMAT;
}
