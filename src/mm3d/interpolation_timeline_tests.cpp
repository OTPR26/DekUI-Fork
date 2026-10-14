// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <stdexcept>
#include "mm3d/interpolation_timeline.h"

using namespace MM3D::InterpolationTimeline;
static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        Submission s;
        s.session = 1;
        s.owner = 123;
        s.requested = 100;
        Check(!Measure(s).valid, "Do not treat an undispatched submission as GPU execution");
        s.worker = 120;
        s.locked = 170;
        s.submitted = 180;
        Check(!Measure(s).valid, "Exclude incomplete tail submissions");
        s.completed = 250;
        auto d = Measure(s);
        Check(d.valid && d.before_worker == 20 && d.queue_lock == 50 && d.submit == 10 &&
                  d.after_submit == 70 && d.total == 150 && d.overlap == 0,
              "Attribute worker delay, contention, submission and completion separately");
        Check(d.total == d.before_worker + d.queue_lock + d.submit + d.after_submit - d.overlap,
              "Timeline accounts for entire Finish duration");

        s.completed = 175;
        d = Measure(s);
        Check(d.valid && d.after_submit == 0 && d.overlap == 5 && d.total == 75,
              "GPU completion may be observed before queue submission returns");
        Check(d.total == d.before_worker + d.queue_lock + d.submit + d.after_submit - d.overlap,
              "Subtract overlapping host observation instead of wrapping unsigned duration");
        s.completed = 150;
        Check(!Measure(s).valid, "Reject completion before the submission lock was acquired");
        s.completed = 250;

        Present p;
        p.session = 1;
        p.owner = 123;
        p.shared_queue = true;
        p.locked = 110;
        p.unlocked = 160;
        Check(PresentLockOverlap(s, p) == 40, "Clip presentation lock hold to worker lock wait");
        p.locked = 170;
        p.unlocked = 200;
        Check(PresentLockOverlap(s, p) == 0, "Touching intervals do not overlap");
        p.locked = 110;
        p.shared_queue = false;
        Check(PresentLockOverlap(s, p) == 0, "A separate presentation queue does not hold graphics lock");
        p.shared_queue = true;
        p.session = 2;
        Check(PresentLockOverlap(s, p) == 0, "Never correlate across restarted captures");
        p.session = 1;
        p.owner = 456;
        Check(PresentLockOverlap(s, p) == 0, "Never correlate different scheduler instances");
        Check(Overlap(20, 10, 0, 30) == 0, "Invalid intervals do not underflow");
        std::cout << "PASS submission timeline: attribution, overlapping completion, incomplete tails, presentation correlation\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
