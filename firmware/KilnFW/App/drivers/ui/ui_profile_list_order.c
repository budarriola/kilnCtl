#include "ui_profile_list_order.h"

void ui_profile_list_order(const uint8_t *ids, size_t count, ui_profile_list_order_is_favorite_fn is_favorite,
                            uint8_t *out)
{
    if (count == 0 || ids == NULL || out == NULL) {
        return;
    }

    size_t w = 0;

    /* Pass 1: favorites, in original order (favs.push in the JS). */
    for (size_t i = 0; i < count; i++) {
        if (is_favorite != NULL && is_favorite(ids[i])) {
            out[w++] = ids[i];
        }
    }
    /* Pass 2: everything else, in original order (rest.push, then
     * favs.concat(rest)). */
    for (size_t i = 0; i < count; i++) {
        if (is_favorite == NULL || !is_favorite(ids[i])) {
            out[w++] = ids[i];
        }
    }
}
