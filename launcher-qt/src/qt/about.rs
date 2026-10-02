//! The About window's model: `launcher_core::about` as properties for
//! the header and one row per credit, with the group's title as the
//! `section` role the view groups by. Nothing here changes after it is
//! built, so there is no reset and no notify beyond the properties'
//! own.

#[cxx_qt::bridge]
pub mod ffi {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
        include!("cxx-qt-lib/qvariant.h");
        type QVariant = cxx_qt_lib::QVariant;
        include!("cxx-qt-lib/qmodelindex.h");
        type QModelIndex = cxx_qt_lib::QModelIndex;
        include!("cxx-qt-lib/qhash.h");
        type QHash_i32_QByteArray = cxx_qt_lib::QHash<cxx_qt_lib::QHashPair_i32_QByteArray>;
    }

    unsafe extern "C++" {
        include!(<QtCore/QAbstractListModel>);
        type QAbstractListModel;
    }

    #[auto_cxx_name]
    extern "RustQt" {
        #[qobject]
        #[base = QAbstractListModel]
        #[qml_element]
        #[qproperty(QString, name)]
        #[qproperty(QString, version)]
        #[qproperty(QString, tagline)]
        #[qproperty(QString, license)]
        #[qproperty(QString, url)]
        #[qproperty(QString, thanks)]
        type AboutModel = super::AboutModelRust;

        #[qinvokable]
        #[cxx_override]
        fn row_count(self: &AboutModel, parent: &QModelIndex) -> i32;

        #[qinvokable]
        #[cxx_override]
        fn data(self: &AboutModel, index: &QModelIndex, role: i32) -> QVariant;

        #[qinvokable]
        #[cxx_override]
        fn role_names(self: &AboutModel) -> QHash_i32_QByteArray;
    }
}

use crate::qs;
use cxx_qt_lib::{QByteArray, QHash, QHashPair_i32_QByteArray, QModelIndex, QString, QVariant};
use launcher_core::about::{self, Credit};

const ROLE_SECTION: i32 = 0x0100;
const ROLE_NAME: i32 = 0x0101;
const ROLE_WHAT: i32 = 0x0102;
const ROLE_LICENSE: i32 = 0x0103;
const ROLE_URL: i32 = 0x0104;

pub struct AboutModelRust {
    name: QString,
    version: QString,
    tagline: QString,
    license: QString,
    url: QString,
    thanks: QString,
    rows: Vec<(&'static str, &'static Credit)>,
}

impl Default for AboutModelRust {
    fn default() -> Self {
        AboutModelRust {
            name: qs(about::NAME),
            version: qs(about::VERSION),
            tagline: qs(about::TAGLINE),
            license: qs(about::LICENSE),
            url: qs(about::URL),
            thanks: qs(about::THANKS),
            rows: about::GROUPS
                .iter()
                .flat_map(|g| g.credits.iter().map(move |c| (g.title, c)))
                .collect(),
        }
    }
}

impl ffi::AboutModel {
    fn row_count(&self, _parent: &QModelIndex) -> i32 {
        self.rows.len() as i32
    }

    fn data(&self, index: &QModelIndex, role: i32) -> QVariant {
        let Some(&(section, c)) = usize::try_from(index.row()).ok().and_then(|r| self.rows.get(r)) else {
            return QVariant::default();
        };
        let text = match role {
            ROLE_SECTION => section,
            ROLE_NAME => c.name,
            ROLE_WHAT => c.what,
            ROLE_LICENSE => c.license,
            ROLE_URL => c.url,
            _ => return QVariant::default(),
        };
        QVariant::from(&qs(text))
    }

    fn role_names(&self) -> QHash<QHashPair_i32_QByteArray> {
        let mut roles = QHash::<QHashPair_i32_QByteArray>::default();
        roles.insert(ROLE_SECTION, QByteArray::from("section"));
        roles.insert(ROLE_NAME, QByteArray::from("name"));
        roles.insert(ROLE_WHAT, QByteArray::from("what"));
        roles.insert(ROLE_LICENSE, QByteArray::from("license"));
        roles.insert(ROLE_URL, QByteArray::from("url"));
        roles
    }
}
