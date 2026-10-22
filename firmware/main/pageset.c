#include "pageset.h"

#include <string.h>

void pageset_reset(pageset_t *ps)
{
    proto_layout_t *pages = ps->pages;
    memset(ps, 0, sizeof(*ps));
    ps->pages = pages;
}

bool pageset_arriving(const pageset_t *ps)
{
    return ps->mask != 0;
}

bool pageset_expired(const pageset_t *ps, int64_t now_ms)
{
    return pageset_arriving(ps) && now_ms - ps->started_ms >= PAGESET_TIMEOUT_MS;
}

static pageset_result_t broken(pageset_t *ps, const char **why, const char *reason)
{
    ps->mask = 0;
    *why = reason;
    return PAGESET_BROKEN;
}

pageset_result_t pageset_add(pageset_t *ps, const proto_layout_t *page, int64_t now_ms, const char **why)
{
    *why = NULL;
    if (page->pages == 0 || page->pages > PROTO_MAX_PAGES || page->page >= page->pages) {
        return broken(ps, why, "page outside its page count"); // proto_parse refuses these already
    }
    if (pageset_arriving(ps)) {
        if (page->rev < ps->rev) {
            return broken(ps, why, "a page of an older rev");
        }
        if (page->rev > ps->rev) {
            ps->mask = 0; // the daemon has moved on: what is left of this set will never come
            *why = "a newer rev discarded the set arriving";
        } else if (page->pages != ps->n_pages) {
            return broken(ps, why, "page counts disagree");
        } else if (ps->mask & (1u << page->page)) {
            return broken(ps, why, "a page came twice");
        } else if (ps->mask != (1u << page->page) - 1) {
            return broken(ps, why, "a page out of order"); // pages come 0 first, so a gap means one was lost
        }
    }
    if (!pageset_arriving(ps)) {
        if (ps->completed && page->rev <= ps->rev) {
            return broken(ps, why, "a rev no newer than the last set");
        }
        if (page->page != 0) {
            return broken(ps, why, "a set that doesn't start at page 0");
        }
        ps->rev = page->rev;
        ps->n_pages = page->pages;
        ps->started_ms = now_ms;
    }
    ps->pages[page->page] = *page;
    ps->mask |= 1u << page->page;
    if (ps->mask != (1u << ps->n_pages) - 1) {
        return PAGESET_PENDING;
    }
    ps->mask = 0;
    ps->completed = true;
    return PAGESET_COMPLETE;
}
