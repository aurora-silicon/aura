/*
 * Replay of the guest TLBI trap count. Above this file run.py has placed, in
 * order: host.h, the production macros, the four-or-more encodings read out of
 * the rewrite in hv_handle_msr_unlocked() (rewrite_forms[]), that handler's own
 * ISS mask statement (handler_reg()), and the production record, counter and
 * report, all verbatim. The expected kinds below come from the Arm ARM's TLBI
 * encodings, not from the code under test.
 *
 * Every check prints "PASS <name>"; the first failure prints "FAIL <name>" on
 * stderr and exits 1.
 */

static unsigned passes;
#define check(cond, name)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s\n", name);                                                    \
            exit(1);                                                                               \
        }                                                                                          \
        fprintf(stdout, "PASS %s\n", name);                                                        \
        passes++;                                                                                  \
    } while (0)

enum kind { K_NONE, K_OS, K_IS, K_OTHER };

/* ESR_ELx.ISS for EC 0x18, from the Arm ARM: op0 21:20, op2 19:17, op1 16:14,
 * CRn 13:10, Rt 9:5, CRm 4:1, direction 0. Built here, not with the macros. */
static u64 iss_of(unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2, unsigned rt,
                  unsigned dir)
{
    return ((u64)op0 << 20) | ((u64)op2 << 17) | ((u64)op1 << 14) | ((u64)crn << 10) |
           ((u64)rt << 5) | ((u64)crm << 1) | dir;
}

static bool rewritten(unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2)
{
    for (unsigned i = 0; i < REWRITE_FORMS; i++)
        if (rewrite_forms[i][0] == op0 && rewrite_forms[i][1] == op1 &&
            rewrite_forms[i][2] == crn && rewrite_forms[i][3] == crm && rewrite_forms[i][4] == op2)
            return true;
    return false;
}

/* TLBI and TLBI nXS are SYS with op0 1 and CRn 8 or 9. "os" is defined as the
 * forms the rewrite handles; EL1 inner-shareable is op1 0, CRn 8, CRm 3 (plain)
 * or CRm 2 (range). */
static enum kind expected(unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2)
{
    if (op0 != 1 || !(crn == 8 || crn == 9))
        return K_NONE;
    if (rewritten(op0, op1, crn, crm, op2))
        return K_OS;
    if (op1 == 0 && crn == 8 && crm >= 2 && crm <= 3)
        return K_IS;
    return K_OTHER;
}

static struct hv_tlbi_traps snap[MAX_CPUS];

static u64 *field_of(struct hv_tlbi_traps *t, enum kind k)
{
    return k == K_OS ? &t->os : k == K_IS ? &t->is : &t->other;
}

/* One trap on CPU `c`: the expected field of record c moves by one, nothing else. */
static bool one_trap_moves_only(int c, u64 iss, enum kind k)
{
    memcpy(snap, hv_tlbi_traps, sizeof(snap));
    model_cpu = c;
    hv_count_tlbi_trap(handler_reg(iss));
    if (k == K_NONE || c < 0 || c >= MAX_CPUS)
        return memcmp(snap, hv_tlbi_traps, sizeof(snap)) == 0;
    (*field_of(&snap[c], k))++;
    return memcmp(snap, hv_tlbi_traps, sizeof(snap)) == 0;
}

static const struct named {
    const char *name;
    unsigned op0, op1, crn, crm, op2;
    enum kind k;
} named[] = {
    {"VMALLE1OS", 1, 0, 8, 1, 0, K_OS},         {"VAE1OS", 1, 0, 8, 1, 1, K_OS},
    {"ASIDE1OS", 1, 0, 8, 1, 2, K_OS},          {"RVAE1OS", 1, 0, 8, 5, 1, K_OS},
    {"VAAE1OS", 1, 0, 8, 1, 3, K_OTHER},        {"VALE1OS", 1, 0, 8, 1, 5, K_OTHER},
    {"VAALE1OS", 1, 0, 8, 1, 7, K_OTHER},       {"RVAAE1OS", 1, 0, 8, 5, 3, K_OTHER},
    {"RVALE1OS", 1, 0, 8, 5, 5, K_OTHER},       {"RVAALE1OS", 1, 0, 8, 5, 7, K_OTHER},
    {"VMALLE1IS", 1, 0, 8, 3, 0, K_IS},         {"VAE1IS", 1, 0, 8, 3, 1, K_IS},
    {"ASIDE1IS", 1, 0, 8, 3, 2, K_IS},          {"VAAE1IS", 1, 0, 8, 3, 3, K_IS},
    {"VALE1IS", 1, 0, 8, 3, 5, K_IS},           {"VAALE1IS", 1, 0, 8, 3, 7, K_IS},
    {"RVAE1IS", 1, 0, 8, 2, 1, K_IS},           {"RVAAE1IS", 1, 0, 8, 2, 3, K_IS},
    {"RVALE1IS", 1, 0, 8, 2, 5, K_IS},          {"RVAALE1IS", 1, 0, 8, 2, 7, K_IS},
    {"VMALLE1", 1, 0, 8, 7, 0, K_OTHER},        {"VAE1", 1, 0, 8, 7, 1, K_OTHER},
    {"ASIDE1", 1, 0, 8, 7, 2, K_OTHER},         {"RVAE1", 1, 0, 8, 6, 1, K_OTHER},
    {"VMALLE1OSNXS", 1, 0, 9, 1, 0, K_OTHER},   {"VMALLE1ISNXS", 1, 0, 9, 3, 0, K_OTHER},
    {"ALLE2OS", 1, 4, 8, 1, 0, K_OTHER},        {"IPAS2E1IS", 1, 4, 8, 0, 1, K_OTHER},
    {"DC CIVAC", 1, 3, 7, 14, 1, K_NONE},       {"IC IALLUIS", 1, 0, 7, 1, 0, K_NONE},
    {"AT S1E1R", 1, 0, 7, 8, 0, K_NONE},        {"ICC_IAR1_EL1", 3, 0, 12, 12, 0, K_NONE},
    {"ID_AA64PFR0_EL1", 3, 0, 0, 4, 0, K_NONE}, {"PMCR_EL0", 3, 3, 9, 12, 0, K_NONE},
    {"CNTV_CTL_EL0", 3, 3, 14, 3, 1, K_NONE},
};

#define THREADS    8
#define PER_THREAD 200000

static void *worker(void *arg)
{
    int c = (int)(intptr_t)arg;
    u64 os = SYSREG_ISS(sys_reg(1, 0, 8, 1, 0)), is = SYSREG_ISS(sys_reg(1, 0, 8, 3, 1)),
        other = SYSREG_ISS(sys_reg(1, 0, 8, 7, 0));

    model_cpu = c;
    for (int n = 0; n < PER_THREAD; n++)
        hv_count_tlbi_trap(n % 3 == 0 ? os : n % 3 == 1 ? is : other);
    return NULL;
}

int main(void)
{
    static const char *const gate[] = {"guest WFIs held at EL2:",
                                       "emulated IRQ traffic (ack/eoi):",
                                       "redirected sync vectors:",
                                       "User interrupt",
                                       "SYSTEM_OFF",
                                       "All CPUs exited",
                                       "now running:",
                                       "guest bugcheck"};

    /* F10: the forms read out of the rewrite are the four named ones, and the
     * named table agrees with them. A change to the rewrite shows up here. */
    unsigned named_os = 0;
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        const struct named *n = &named[i];
        if (n->k == K_OS) {
            named_os++;
            check(rewritten(n->op0, n->op1, n->crn, n->crm, n->op2), n->name);
        }
        check(expected(n->op0, n->op1, n->crn, n->crm, n->op2) == n->k, n->name);
    }
    check(REWRITE_FORMS == named_os, "the rewrite handles exactly the four named OS forms");

    /* F11, F12, F14: every encoding, both directions, three Rt values, spread over
     * all CPUs: exactly the expected field of the trapping CPU moves by one. */
    memset(hv_tlbi_traps, 0, sizeof(hv_tlbi_traps));
    u64 totals[4] = {0};
    unsigned sweep = 0;
    bool sweep_ok = true;
    for (unsigned op0 = 0; op0 < 4; op0++)
        for (unsigned op1 = 0; op1 < 8; op1++)
            for (unsigned crn = 0; crn < 16; crn++)
                for (unsigned crm = 0; crm < 16; crm++)
                    for (unsigned op2 = 0; op2 < 8; op2++)
                        for (unsigned dir = 0; dir < 2; dir++)
                            for (unsigned r = 0; r < 3; r++) {
                                unsigned rt = r == 0 ? 0 : r == 1 ? 17 : 31;
                                enum kind k = expected(op0, op1, crn, crm, op2);
                                int c = (int)(sweep++ % MAX_CPUS);
                                totals[k]++;
                                if (!one_trap_moves_only(
                                        c, iss_of(op0, op1, crn, crm, op2, rt, dir), k)) {
                                    fprintf(stderr, "mismatch op0 %u op1 %u CRn %u CRm %u op2 %u\n",
                                            op0, op1, crn, crm, op2);
                                    sweep_ok = false;
                                }
                            }
    check(sweep == 4 * 8 * 16 * 16 * 8 * 2 * 3, "sweep covered 98304 traps");
    check(sweep_ok, "every encoding counted once, as its kind, on its own CPU only");
    check(totals[K_OS] == 4 * 6 && totals[K_IS] == 16 * 6 && totals[K_OTHER] == (2048 - 20) * 6,
          "4 os, 16 is and 2028 other TLBI encodings; the other 14336 are not TLBIs");

    /* F5: a CPU number out of range writes nothing anywhere. */
    u64 vmalle1os = iss_of(1, 0, 8, 1, 0, 0, 0);
    check(one_trap_moves_only(MAX_CPUS, vmalle1os, K_OS), "cpu == MAX_CPUS is not counted");
    check(one_trap_moves_only(MAX_CPUS + 7, vmalle1os, K_OS), "cpu > MAX_CPUS is not counted");
    check(one_trap_moves_only(-1, vmalle1os, K_OS), "a negative cpu is not counted");

    /* F4: each CPU's record is its own 64-byte line. */
    check(_Alignof(struct hv_tlbi_traps) >= 64 && sizeof(struct hv_tlbi_traps) % 64 == 0 &&
              ((uintptr_t)&hv_tlbi_traps[0] % 64) == 0,
          "one 64-byte-aligned record per CPU");

    /* F14: real concurrent writers lose nothing. */
    memset(hv_tlbi_traps, 0, sizeof(hv_tlbi_traps));
    pthread_t threads[THREADS];
    for (int t = 0; t < THREADS; t++)
        check(pthread_create(&threads[t], NULL, worker, (void *)(intptr_t)(t * 9 % MAX_CPUS)) == 0,
              "thread started");
    for (int t = 0; t < THREADS; t++)
        pthread_join(threads[t], NULL);
    bool threads_ok = true;
    for (int c = 0; c < MAX_CPUS; c++) {
        bool used = false;
        for (int t = 0; t < THREADS; t++)
            used |= c == t * 9 % MAX_CPUS;
        /* n % 3 == 0, 1, 2 over n = 0 .. PER_THREAD - 1 */
        u64 os = used ? (PER_THREAD + 2) / 3 : 0, is = used ? (PER_THREAD + 1) / 3 : 0,
            other = used ? PER_THREAD / 3 : 0;
        if (hv_tlbi_traps[c].os != os || hv_tlbi_traps[c].is != is ||
            hv_tlbi_traps[c].other != other)
            threads_ok = false;
    }
    check(threads_ok, "8 concurrent CPUs x 200000 traps: every count exact");

    /* F7, F9, F19: one line, once, parsed back, full 64-bit values, no gate text. */
    memset(hv_tlbi_traps, 0, sizeof(hv_tlbi_traps));
    u64 want_os = 0, want_is = 0, want_other = 0;
    for (int c = 0; c < MAX_CPUS; c++) {
        hv_tlbi_traps[c].os = 3 * c + 1;
        hv_tlbi_traps[c].is = 5 * c;
        hv_tlbi_traps[c].other = c == MAX_CPUS - 1 ? (1UL << 40) + 12345 : 7;
        want_os += hv_tlbi_traps[c].os;
        want_is += hv_tlbi_traps[c].is;
        want_other += hv_tlbi_traps[c].other;
    }
    console_len = 0;
    printf_calls = 0;
    hv_report_tlbi_traps();
    check(printf_calls == 1, "one printf");
    unsigned long got_os, got_is, got_other;
    int consumed = 0;
    int fields = sscanf(console, "HV TLBI TRAPS os=%lu is=%lu other=%lu\n%n", &got_os, &got_is,
                        &got_other, &consumed);
    check(fields == 3, "line parses with the fixed prefix and three fields");
    check(got_os == want_os && got_is == want_is && got_other == want_other,
          "totals are the sums over every CPU, 64-bit values in full");
    check((size_t)consumed == console_len && console[console_len - 1] == '\n' && console_len < 512,
          "exactly one newline-terminated line under 512 bytes");
    check(strncmp(console, "HV TLBI TRAPS ", 14) == 0, "fixed greppable prefix");
    bool clean = true;
    for (unsigned g = 0; g < sizeof(gate) / sizeof(gate[0]); g++)
        clean &= strstr(console, gate[g]) == NULL;
    check(clean, "line carries no substring the health gate parses");
    size_t first_len = console_len;
    hv_report_tlbi_traps();
    check(printf_calls == 1 && console_len == first_len, "a second call prints nothing");

    fprintf(stdout, "CONSOLE\n%sCHECKS %u\n", console, passes);
    return 0;
}
