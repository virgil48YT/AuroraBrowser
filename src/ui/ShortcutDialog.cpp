#include "ui/ShortcutDialog.hpp"
#include "ui/ThemeState.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aurora::ui {

bool prompt_shortcut(QWidget* parent, const QString& heading,
                     const Shortcut& initial, Shortcut* out) {
    if (!out) return false;
    QDialog dialog(parent);
    dialog.setWindowTitle(heading);
    dialog.setModal(true);
    dialog.setMinimumWidth(380);

    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* name = new QLineEdit(initial.title, &dialog);
    name->setPlaceholderText(QStringLiteral("Name (optional)"));
    auto* address = new QLineEdit(initial.url, &dialog);
    address->setPlaceholderText(QStringLiteral("example.com"));
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Address"), address);
    layout->addLayout(form);

    auto* error = new QLabel(&dialog);
    // Theme danger colour: the old fixed light red was unreadable on white.
    error->setStyleSheet(QStringLiteral("color: %1;").arg(current_theme().colors.danger.name()));
    error->hide();
    layout->addWidget(error);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Save"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (normalize_shortcut_url(address->text()).isEmpty()) {
            error->setText(QStringLiteral("Enter a valid web address."));
            error->show();
            address->setFocus();
            return;
        }
        dialog.accept();
    });
    address->setFocus();

    if (dialog.exec() != QDialog::Accepted) return false;
    out->url = normalize_shortcut_url(address->text());
    out->title = name->text().trimmed();
    if (out->title.isEmpty()) out->title = default_shortcut_title(out->url);
    return true;
}

} // namespace aurora::ui
