/* bf6_type_census - count INSTANCES of a type across a level, from the install.
 *
 * WHY THIS EXISTS. Several systems turn out to author nothing a level carries:
 * mesh-compute has 66 reflected types and no instances, virtual texturing 2,
 * framegraph 2, ambient shadows 2. "We looked and found nothing" is the least
 * trustworthy claim in this repository, so it needs to be a MEASUREMENT anyone
 * can re-run against their own install rather than a note in a document.
 *
 * It counts through the partition's own type table - `instance_type` - and not
 * by searching raw bytes for the GUID. A raw search finds the type TABLE of any
 * partition that merely references the type and reports a hit where there is no
 * instance; that is a candidate finder, and this is a census.
 *
 * A ZERO IS ONLY WORTH ANYTHING NEXT TO A NON-ZERO, so the caller is expected
 * to run a control type it knows ships. `partitions_parsed` is reported for the
 * same reason: zero instances over zero partitions means the mount failed, not
 * that the data is absent.
 */

bf6_type_census_result bf6_type_census(bf6_ctx* c, const char* level,
                                       const char* type_guid_hex)
{
    bf6_type_census_result r{};
    if (!c || !type_guid_hex) return r;

    /* TEXTUAL form -> STORED bytes: first three groups byte-reversed. Both
     * spellings are matched, because getting this wrong reports a shipped type
     * as absent, which is exactly the mistake this function exists to prevent. */
    uint8_t raw[16] = {0};
    int n = 0;
    for (const char* q = type_guid_hex; *q && n < 16; ) {
        if (*q == '-') { q++; continue; }
        if (!q[1]) break;
        char hx[3] = { q[0], q[1], 0 };
        raw[n++] = (uint8_t)strtoul(hx, nullptr, 16);
        q += 2;
    }
    if (n != 16) return r;
    uint8_t swapped[16];
    swapped[0]=raw[3]; swapped[1]=raw[2]; swapped[2]=raw[1]; swapped[3]=raw[0];
    swapped[4]=raw[5]; swapped[5]=raw[4];
    swapped[6]=raw[7]; swapped[7]=raw[6];
    for (int k = 8; k < 16; k++) swapped[k] = raw[k];

    std::string terr;
    if (!ensure_types(c, terr)) return r;

    const std::string want = level ? level : "";
    for (const auto& kv : c->src.ebx()) {
        if (!want.empty() && kv.first.find(want) == std::string::npos) continue;
        r.partitions_matched++;
        std::string e;
        std::vector<uint8_t> b = c->src.get_ebx(kv.first, e);
        if (b.empty()) continue;
        bf6::Ebx x(*c->types);
        if (!x.parse(std::move(b), e)) continue;
        r.partitions_parsed++;
        for (size_t i = 0; i < x.instance_count(); i++) {
            const bf6::TypeGuid g = x.instance_type(i);
            if (memcmp(g.data(), raw, 16) == 0 || memcmp(g.data(), swapped, 16) == 0) {
                r.instances++;
                if (r.partitions_with == 0 ||
                    r.last_partition[0] == 0) {
                    const size_t len = kv.first.size() < sizeof(r.last_partition) - 1
                                     ? kv.first.size() : sizeof(r.last_partition) - 1;
                    memcpy(r.last_partition, kv.first.data(), len);
                    r.last_partition[len] = 0;
                }
            }
        }
        bool here = false;
        for (size_t i = 0; i < x.instance_count() && !here; i++) {
            const bf6::TypeGuid g = x.instance_type(i);
            here = memcmp(g.data(), raw, 16) == 0 || memcmp(g.data(), swapped, 16) == 0;
        }
        if (here) r.partitions_with++;
    }
    return r;
}
