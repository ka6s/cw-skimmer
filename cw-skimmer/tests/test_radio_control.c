#include "radio_control.h"

#include <stdio.h>
#include <string.h>

static int g_fails;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fails++;
    }
}

int main(void)
{
    char clean[64];
    char dds[64];
    char vfo[64];
    char cmd[128];
    char keyer[32];
    char macros[32];
    char chunk[40];
    int pos = 0;
    int n;

    n = radio_control_sanitize_cw("cq de test;", clean, (int)sizeof(clean));
    expect(n == 10 && strcmp(clean, "CQ DE TEST") == 0, "strip semicolon and upper-case");

    n = radio_control_sanitize_cw("hi:there", clean, (int)sizeof(clean));
    expect(strcmp(clean, "HITHERE") == 0, "drop colon");

    n = radio_control_sanitize_cw("ok?", clean, (int)sizeof(clean));
    expect(strcmp(clean, "OK?") == 0, "keep question mark");

    expect(radio_control_format_tune(0, 14076000LL, dds, (int)sizeof(dds),
                                     vfo, (int)sizeof(vfo)) == 0,
           "tune format");
    expect(strcmp(dds, "dds:0,14076000;") == 0, "dds command");
    expect(strcmp(vfo, "vfo:0,0,14076000;") == 0, "vfo command");
    expect(radio_control_format_tune(0, 10, dds, (int)sizeof(dds),
                                     vfo, (int)sizeof(vfo)) != 0,
           "reject a tiny frequency");

    n = radio_control_format_cw_macro(0, "cq de n0call;", cmd, (int)sizeof(cmd));
    expect(n > 0 && strcmp(cmd, "cw_macros:0,CQ DE N0CALL;") == 0, "cw_macros text");

    expect(radio_control_format_wpm(25, keyer, (int)sizeof(keyer),
                                    macros, (int)sizeof(macros)) == 25,
           "wpm");
    expect(strcmp(keyer, "cw_keyer_speed:25;") == 0, "keyer speed");
    expect(strcmp(macros, "cw_macros_speed:25;") == 0, "macro speed");

    pos = 0;
    expect(radio_control_ky_chunk("ABCDEFGHIJKLMNOPQRSTUVWXYZ1234", &pos,
                                  chunk, (int)sizeof(chunk)) == 1,
           "first KY chunk");
    expect(pos == 24 && strcmp(chunk, "KY ABCDEFGHIJKLMNOPQRSTUVWX;") == 0, "24 character KY");
    expect(radio_control_ky_chunk("ABCDEFGHIJKLMNOPQRSTUVWXYZ1234", &pos,
                                  chunk, (int)sizeof(chunk)) == 1,
           "second KY chunk");
    expect(strcmp(chunk, "KY YZ1234;") == 0, "KY tail");

    expect(radio_control_ky_status("KY0;") == 0, "KY free");
    expect(radio_control_ky_status("KY1;") == 1, "KY busy");
    expect(radio_control_ky_status("KY CQ;") == -1, "KY text is not a status");

    if (g_fails) {
        fprintf(stderr, "%d radio-control checks failed\n", g_fails);
        return 1;
    }
    printf("radio control ok\n");
    return 0;
}
