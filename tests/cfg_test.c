/*
 * cfg_test.c - Unit Tests for VFRS Next-Gen Configuration & Digit Analysis Engine
 * Virtual Frame Relay Switch
 */

#include "vfr.h"
#include "vfr/config.h"
#include "vfr/cfg_lexer.h"
#include "vfr/cfg_ast.h"
#include "vfr/cfg_schema.h"
#include "ports/svc_numbering/svc_numbering.h"
#include "switching/svc_routing_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
        g_tests_failed++; \
    } \
} while(0)

/* ------------------------------------------------------------
 * Test 1: Stream Lexer, Line Continuation, Comments, Key-Val
 * ------------------------------------------------------------ */
static void test_lexer(void)
{
    printf("\n=== Test 1: Stream Lexer & Tokenizer ===\n");

    const char *sample = 
        "# Leading comment\n"
        "port uni0/1 udp 127.0.0.1 30001 \\\n"
        "    127.0.0.1 30002 dlcibit=23 ar=2M\n"
        "log file \"logs/test vfrs.log\"\n";

    cfg_lexer_t *lex = cfg_lexer_create_string(sample, "test1");
    assert(lex != NULL);

    cfg_token_t tok;

    /* Skip any leading newlines from comments */
    do {
        cfg_lexer_next_token(lex, &tok);
    } while (tok.type == TOK_NEWLINE);

    /* Line 1 (after comment): port */
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "port") == 0, "Token 1 is 'port'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "uni0/1") == 0, "Token 2 is 'uni0/1'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "udp") == 0, "Token 3 is 'udp'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "127.0.0.1") == 0, "Token 4 is '127.0.0.1'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "30001") == 0, "Token 5 is '30001'");

    /* Line continuation seamlessly gives next tokens on same logical line */
    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "127.0.0.1") == 0, "Line continuation token 6 is '127.0.0.1'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "30002") == 0, "Token 7 is '30002'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_KEYVAL && strcmp(tok.key, "dlcibit") == 0 && strcmp(tok.val, "23") == 0, "Token 8 is keyval dlcibit=23");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_KEYVAL && strcmp(tok.key, "ar") == 0 && strcmp(tok.val, "2M") == 0, "Token 9 is keyval ar=2M");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_NEWLINE, "Statement 1 terminator is NEWLINE");

    /* Line 2: log file "logs/test vfrs.log" */
    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "log") == 0, "Token 10 is 'log'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_WORD && strcmp(tok.text, "file") == 0, "Token 11 is 'file'");

    cfg_lexer_next_token(lex, &tok);
    TEST_ASSERT(tok.type == TOK_STRING && strcmp(tok.text, "logs/test vfrs.log") == 0, "Token 12 quoted string preserved spaces");

    cfg_lexer_destroy(lex);
}

/* ------------------------------------------------------------
 * Test 2: Strict Schema Parsing Primitives
 * ------------------------------------------------------------ */
static void test_schema_parsers(void)
{
    printf("\n=== Test 2: Strict Schema Parsers ===\n");

    u32 rate = 0;
    TEST_ASSERT(cfg_parse_rate("64000", &rate) == 0 && rate == 64000, "Parse rate 64000 bps");
    TEST_ASSERT(cfg_parse_rate("64k", &rate) == 0 && rate == 64000, "Parse rate 64k");
    TEST_ASSERT(cfg_parse_rate("2M", &rate) == 0 && rate == 2000000, "Parse rate 2M");
    TEST_ASSERT(cfg_parse_rate("-100", &rate) != 0, "Reject negative rate -100");
    TEST_ASSERT(cfg_parse_rate("invalid", &rate) != 0, "Reject non-numeric rate");

    u32 time_ms = 0;
    TEST_ASSERT(cfg_parse_time_ms("1500ms", &time_ms) == 0 && time_ms == 1500, "Parse time 1500ms");
    TEST_ASSERT(cfg_parse_time_ms("1.5s", &time_ms) == 0 && time_ms == 1500, "Parse time 1.5s");
    TEST_ASSERT(cfg_parse_time_ms("2", &time_ms) == 0 && time_ms == 2000, "Parse time 2 (default sec -> 2000ms)");

    int bval = 0;
    TEST_ASSERT(cfg_parse_bool("allow", &bval) == 0 && bval == 1, "Parse bool allow -> 1");
    TEST_ASSERT(cfg_parse_bool("deny", &bval) == 0 && bval == 0, "Parse bool deny -> 0");
    TEST_ASSERT(cfg_parse_bool("enable", &bval) == 0 && bval == 1, "Parse bool enable -> 1");
    TEST_ASSERT(cfg_parse_bool("disable", &bval) == 0 && bval == 0, "Parse bool disable -> 0");

    u32 uval = 0;
    TEST_ASSERT(cfg_parse_u32("1024", 16, 8388607, &uval) == 0 && uval == 1024, "Parse u32 in range");
    TEST_ASSERT(cfg_parse_u32("10", 16, 8388607, &uval) != 0, "Reject u32 below minimum");
    TEST_ASSERT(cfg_parse_u32("100abc", 0, 1000, &uval) != 0, "Reject u32 with trailing junk");
}

/* ------------------------------------------------------------
 * Test 3: Digit Analysis Tree (Digit Trie)
 * ------------------------------------------------------------ */
static void test_digit_trie(void)
{
    printf("\n=== Test 3: Digit Analysis Tree (Digit Trie) ===\n");

    vfr_digit_node_t *root = NULL;

    /* Insert stage-based routes */
    /* 1. Regional Route: DNIC 5104 + SGC 01 -> port nni0/1 */
    svc_trie_insert(&root, "510401", ROUTE_ACTION_NNI, "nni0/1", NULL, 10, 1);

    /* 2. Specific Sub-Switch Route: DNIC 5104 + SGC 01 + SIC 02 -> port nni0/2 */
    svc_trie_insert(&root, "51040102", ROUTE_ACTION_NNI, "nni0/2", NULL, 5, 1);

    /* 3. International Transit Route: Country Code 60 (Malaysia) -> port intl_gw */
    svc_trie_insert(&root, "60", ROUTE_ACTION_NNI, "intl_gw", "MY01", 20, 1);

    /* Test 1: Match 510401021234 -> should match most specific (51040102 -> nni0/2) */
    const vfr_digit_node_t *match = svc_trie_lookup(root, "510401021234");
    TEST_ASSERT(match != NULL && strcmp(match->target_port_name, "nni0/2") == 0,
                "Digit Trie: 510401021234 matches longest prefix 51040102 (nni0/2)");

    /* Test 2: Match 510401059999 -> should match regional route 510401 (nni0/1) */
    match = svc_trie_lookup(root, "510401059999");
    TEST_ASSERT(match != NULL && strcmp(match->target_port_name, "nni0/1") == 0,
                "Digit Trie: 510401059999 matches fallback prefix 510401 (nni0/1)");

    /* Test 3: Match 60123456789 -> should match international transit 60 (intl_gw) */
    match = svc_trie_lookup(root, "60123456789");
    TEST_ASSERT(match != NULL && strcmp(match->target_port_name, "intl_gw") == 0,
                "Digit Trie: 60123456789 matches country code 60 (intl_gw)");

    /* Test 4: Unallocated number 999123 -> no match */
    match = svc_trie_lookup(root, "999123");
    TEST_ASSERT(match == NULL, "Digit Trie: Unallocated number 999123 returns no match");
}

/* ------------------------------------------------------------
 * Test 4: End-to-End Config AST Compilation & Defaults Cascade
 * ------------------------------------------------------------ */
static void test_config_compilation(void)
{
    printf("\n=== Test 4: End-to-End AST Compilation & Defaults Cascade ===\n");

    const char *cfg_str = 
        "swconfig swid=vfrs_test nspf=x121 dnic=5104 sgclen=2 sgc=01 siclen=2 sic=01 sublen=4\n"
        "log level con=warn txt=debug\n"
        "default interface dlcibit=23 ar=10M\n"
        "default pvc cir=1M bc=1000000 be=500000 ftp=10 fdp=5 class=2\n"
        "default svc cir=2M bc=2000000 be=1000000 fmif=1600 revchg=allow\n"
        "port uni0/1 udp-server 127.0.0.1 31001\n"
        "port uni0/2 udp-server 127.0.0.1 31002 dlcibit=10\n"
        "pvc uni0/1 100 uni0/2 200\n"
        "svc int uni0/1 dlci_low=512 dlci_high=991\n"
        "svc addr uni0/1 autoprefix x121 0001\n"
        "svc addr uni0/2 autonumber x121 global\n"
        "svc route uni0/1 x121 dnic=5104 sgc=01 sic=01\n";

    vfrs_ctx_t *ctx = vfrs_create("test_sw");
    assert(ctx != NULL);
    svc_numbering_init(ctx);
    svc_route_init(ctx);

    int rc = cfg_compile_string(ctx, cfg_str, "test_config");
    TEST_ASSERT(rc == 0, "Compiled new specification configuration string with 0 errors");

    /* Validate swconfig */
    TEST_ASSERT(ctx->dnic == 5104, "Switch DNIC compiled to 5104");
    TEST_ASSERT(ctx->sgclen == 2 && ctx->sgc == 1, "Switch SGC compiled to 01");
    TEST_ASSERT(ctx->siclen == 2 && ctx->sic == 1, "Switch SIC compiled to 01");
    TEST_ASSERT(ctx->subnumlen == 4, "Switch sublen compiled to 4");

    /* Validate ports and inherited defaults */
    TEST_ASSERT(ctx->port_count == 2, "Created 2 ports");
    if (ctx->port_count >= 2) {
        TEST_ASSERT(ctx->ports[0]->dlcibit == 23, "uni0/1 inherited default interface dlcibit=23");
        TEST_ASSERT(ctx->ports[1]->dlcibit == 10, "uni0/2 overrode dlcibit=10");
    }

    /* Validate numbering registrations */
    TEST_ASSERT(ctx->svc_subscriber_count >= 2, "Registered SVC subscriber numbers");
    vfr_port_t *sub1 = svc_find_subscriber(ctx, "510401010001");
    TEST_ASSERT(sub1 != NULL && strcmp(sub1->name, "uni0/1") == 0,
                "Autoprefix registered 510401010001 -> uni0/1");

    vfr_port_t *sub2 = svc_find_subscriber(ctx, "510401010002");
    TEST_ASSERT(sub2 != NULL && strcmp(sub2->name, "uni0/2") == 0,
                "Autonumber global sequential registered 510401010002 -> uni0/2");

    /* Validate reserved all-zeros number detection */
    TEST_ASSERT(svc_numbering_is_all_zeros(ctx, "510401010000") == 1,
                "510401010000 identified as reserved all-zeros system number");
    TEST_ASSERT(svc_numbering_is_all_zeros(ctx, "510401010001") == 0,
                "510401010001 identified as valid subscriber number");

    vfrs_destroy(ctx);
}

int main(void)
{
    winsock_init();

    printf("===============================================================\n");
    printf("VFRS Configuration Parser & Digit Analysis Unit Test Suite\n");
    printf("===============================================================\n");

    test_lexer();
    test_schema_parsers();
    test_digit_trie();
    test_config_compilation();

    printf("\n===============================================================\n");
    printf("TEST RESULTS: %d Passed, %d Failed\n", g_tests_passed, g_tests_failed);
    printf("===============================================================\n");

    winsock_shutdown();
    return (g_tests_failed == 0) ? 0 : 1;
}
