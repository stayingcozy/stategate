use posture::gen::fake::FakeHal;
use posture::gen::hal::SafeHal;
use posture::gen::machine::Event;
use posture::services::imu_posture::ImuPosture;

#[test]
fn stim_off_sets_intensity_to_zero() {
    let (fake, probe) = FakeHal::new();
    let mut hal = SafeHal::new(fake);
    posture::actions::stim_off::run(&mut hal);
    assert_eq!(probe.borrow().last("stim_set_intensity"), Some(0));
}

#[test]
fn imu_stub_reports_one_slouch_at_10_s() {
    let (fake, probe) = FakeHal::new();
    let mut hal = SafeHal::new(fake);
    let mut imu = ImuPosture::default();
    probe.borrow_mut().now_ms = 9_990;
    assert_eq!(imu.poll(&mut hal), None);
    probe.borrow_mut().now_ms = 10_000;
    assert_eq!(imu.poll(&mut hal), Some(Event::Slouch));
    assert_eq!(imu.poll(&mut hal), None);
}
