#include "ota_progress.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void)
{
    OtaSnapshot snapshot = {0};
    for (int percent = 0; percent <= 100; ++percent) {
        ota_snapshot_progress(&snapshot, "transferring", percent,
                              ota_collector_overall_progress(percent));
        assert(snapshot.stage_progress == percent);
        assert(snapshot.progress == 70 + percent * 29 / 100);
        assert(snapshot.overall_progress == snapshot.progress);
    }
    snapshot.state = OTA_STATE_INSTALLING;
    ota_snapshot_progress(&snapshot, "verifying", 100, 99);
    assert(snapshot.state == OTA_STATE_INSTALLING);
    assert(strcmp(snapshot.stage, "verifying") == 0);
    ota_snapshot_progress(&snapshot, "installing", 0, 99);
    assert(snapshot.stage_progress == 0);
    assert(snapshot.progress == 99);
    for (OtaTarget target = OTA_TARGET_ESP; target <= OTA_TARGET_BMS; ++target) {
        int previous = 0;
        for (int percent = 0; percent <= 100; ++percent) {
            ota_snapshot_progress(&snapshot, "downloading", percent,
                                  ota_receiving_overall_progress(target, percent));
            assert(snapshot.stage_progress == percent);
            assert(snapshot.overall_progress >= previous);
            assert(snapshot.overall_progress < 100);
            previous = snapshot.overall_progress;
        }
        assert(previous == (target == OTA_TARGET_ESP ? 95 : 70));
        if (target != OTA_TARGET_ESP) {
            for (int percent = 0; percent <= 100; ++percent) {
                int overall = ota_collector_overall_progress(percent);
                assert(overall >= previous && overall < 100);
                previous = overall;
            }
        }
    }
    assert(ota_collector_overall_progress(-1) == 70);
    assert(ota_collector_overall_progress(101) == 99);
    return 0;
}
