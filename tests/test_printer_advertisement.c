#include "printer_advertisement.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *value_for(const printer_txt_item_t *items, size_t count,
                             const char *key)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(items[i].key, key) == 0) {
            return items[i].value;
        }
    }
    return NULL;
}

static void test_builds_truthful_airprint_record(void)
{
    printer_target_t target = {0};
    snprintf(target.label, sizeof(target.label), "TestCo Legacy 500");
    snprintf(target.pdl, sizeof(target.pdl), "image/urf,application/pdf");
    snprintf(target.urf, sizeof(target.urf), "W8,SRGB24,RS600");
    snprintf(target.location, sizeof(target.location), "Studio");
    target.color = true;
    target.duplex = false;
    target.copies = true;
    target.collate = false;
    target.printer_state = 4;

    printer_advertisement_t advertisement;
    printer_advertisement_build(&target, "bridge-test-uuid", NULL,
                                &advertisement);
    printer_txt_item_t items[ESPRESSO_DNSSD_TXT_MAX];
    size_t count = printer_advertisement_txt(
        &advertisement, items, ESPRESSO_DNSSD_TXT_MAX);

    assert(count == ESPRESSO_DNSSD_TXT_MAX);
    assert(strcmp(ESPRESSO_DNSSD_SERVICE, "_ipp") == 0);
    assert(strcmp(ESPRESSO_DNSSD_PROTOCOL, "_tcp") == 0);
    assert(strcmp(ESPRESSO_DNSSD_SUBTYPE, "_universal") == 0);
    assert(strcmp(advertisement.instance,
                  "ESPresso - TestCo Legacy 500 (uuid)") == 0);
    assert(strcmp(value_for(items, count, "rp"), "ipp/print") == 0);
    assert(strcmp(value_for(items, count, "pdl"), target.pdl) == 0);
    assert(strcmp(value_for(items, count, "URF"), target.urf) == 0);
    assert(strcmp(value_for(items, count, "Color"), "T") == 0);
    assert(strcmp(value_for(items, count, "Duplex"), "F") == 0);
    assert(strcmp(value_for(items, count, "Copies"), "T") == 0);
    assert(strcmp(value_for(items, count, "Collate"), "F") == 0);
    assert(strcmp(value_for(items, count, "printer-state"), "4") == 0);
    assert(strcmp(value_for(items, count, "UUID"), "bridge-test-uuid") == 0);
    assert(strcmp(value_for(items, count, "note"), "Studio") == 0);
    assert(strcmp(value_for(items, count, "adminurl"),
                  "http://espresso.local/") == 0);
}

static void test_bridge_identity_avoids_service_name_collisions(void)
{
    printer_target_t target = {0};
    snprintf(target.label, sizeof(target.label), "Shared Printer");
    printer_advertisement_t first;
    printer_advertisement_t second;
    printer_advertisement_build(
        &target, "00000000-0000-4000-8000-00000000a123", NULL, &first);
    printer_advertisement_build(
        &target, "00000000-0000-4000-8000-00000000b456", NULL, &second);
    assert(strcmp(first.instance, second.instance) != 0);
    assert(strstr(first.instance, "(a123)") != NULL);
    assert(strstr(second.instance, "(b456)") != NULL);
}

static void test_uses_conservative_defaults(void)
{
    printer_target_t target = {0};
    snprintf(target.instance, sizeof(target.instance), "Fallback Printer");
    snprintf(target.pdl, sizeof(target.pdl), "image/urf");
    snprintf(target.urf, sizeof(target.urf), "W8,RS300");
    printer_advertisement_t advertisement;
    printer_advertisement_build(&target, "", NULL, &advertisement);
    printer_txt_item_t items[ESPRESSO_DNSSD_TXT_MAX];
    size_t count = printer_advertisement_txt(
        &advertisement, items, ESPRESSO_DNSSD_TXT_MAX);
    assert(strcmp(value_for(items, count, "Color"), "F") == 0);
    assert(strcmp(value_for(items, count, "Duplex"), "F") == 0);
    assert(strcmp(value_for(items, count, "printer-state"), "3") == 0);
    assert(strcmp(value_for(items, count, "note"),
                  "Legacy printer bridged by ESPresso") == 0);
    assert(strstr(value_for(items, count, "pdl"), "image/pwg-raster") == NULL);
}

static void test_custom_name_replaces_generated_service_name(void)
{
    printer_target_t target = {0};
    snprintf(target.label, sizeof(target.label), "TestCo Legacy 500");
    printer_advertisement_t advertisement;
    printer_advertisement_build(&target, "bridge-test-uuid", "Office Printer",
                                &advertisement);
    assert(strcmp(advertisement.instance, "Office Printer") == 0);
    assert(strcmp(advertisement.make_model, "TestCo Legacy 500") == 0);
}

int main(void)
{
    test_builds_truthful_airprint_record();
    test_uses_conservative_defaults();
    test_bridge_identity_avoids_service_name_collisions();
    test_custom_name_replaces_generated_service_name();
    puts("DNS-SD advertisement tests passed");
    return 0;
}
