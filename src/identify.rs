//! Vendor identification for the cameras this tool is aimed at.
//!
//! Orbbec's USB vendor ID is 0x2BC5. Intel uses 0x8086 for many unrelated
//! devices, so a RealSense match also requires a known product ID or a product
//! string containing "realsense". Catalog names are hints; the device string
//! is the name to trust when it is present.

pub const ORBBEC_VID: u16 = 0x2BC5;
pub const INTEL_VID: u16 = 0x8086;

/// Product IDs published by librealsense for depth cameras. The list is not
/// exhaustive. Unknown RealSense IDs still match when the product string says so.
const REALSENSE_PIDS: &[(u16, &str)] = &[
    (0x0AD1, "D400"),
    (0x0AD2, "D410"),
    (0x0AD3, "D415"),
    (0x0AD4, "D430"),
    (0x0AD5, "D430"),
    (0x0AD6, "D420"),
    (0x0AF6, "D420"),
    (0x0AFE, "D410"),
    (0x0AFF, "D400"),
    (0x0B00, "D430"),
    (0x0B01, "D460"),
    (0x0B03, "D435"),
    (0x0B07, "D435"),
    (0x0B0C, "D405"),
    (0x0B3A, "D435i"),
    (0x0B5B, "D405"),
    (0x0B5C, "D455"),
    (0x0B64, "L515"),
];

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CameraIdentity {
    pub family: &'static str,
    pub catalog_name: Option<&'static str>,
}

pub fn identify_camera(
    vendor_id: u16,
    product_id: u16,
    product: Option<&str>,
) -> Option<CameraIdentity> {
    if vendor_id == ORBBEC_VID {
        return Some(CameraIdentity {
            family: "orbbec",
            catalog_name: None,
        });
    }
    let string_match = product
        .map(|text| text.to_ascii_lowercase().contains("realsense"))
        .unwrap_or(false);
    let catalog = REALSENSE_PIDS
        .iter()
        .find(|(pid, _)| *pid == product_id)
        .map(|(_, name)| *name);
    if vendor_id == INTEL_VID && (string_match || catalog.is_some()) {
        return Some(CameraIdentity {
            family: "realsense",
            catalog_name: catalog,
        });
    }
    None
}

pub fn class_name(class: u8, subclass: u8) -> &'static str {
    match (class, subclass) {
        (0x00, _) => "use-interface",
        (0x01, _) => "audio",
        (0x02, _) => "cdc-control",
        (0x03, _) => "hid",
        (0x08, _) => "mass-storage",
        (0x09, _) => "hub",
        (0x0A, _) => "cdc-data",
        (0x0E, 0x01) => "video-control",
        (0x0E, 0x02) => "video-streaming",
        (0x0E, _) => "video",
        (0x0B, _) => "billboard-or-misc",
        (0xEF, _) => "miscellaneous",
        (0xFF, _) => "vendor",
        _ => "other",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn orbbec_vendor_matches_without_a_product_string() {
        let id = identify_camera(ORBBEC_VID, 0x0670, None).unwrap();
        assert_eq!(id.family, "orbbec");
    }

    #[test]
    fn intel_wifi_is_not_a_realsense() {
        assert!(identify_camera(INTEL_VID, 0x2723, Some("Wi-Fi 6 AX200")).is_none());
    }

    #[test]
    fn known_pid_and_product_string_match_realsense() {
        assert_eq!(
            identify_camera(INTEL_VID, 0x0B3A, None)
                .unwrap()
                .catalog_name,
            Some("D435i")
        );
        assert_eq!(
            identify_camera(INTEL_VID, 0x1111, Some("Intel RealSense D999"))
                .unwrap()
                .family,
            "realsense"
        );
    }
}
