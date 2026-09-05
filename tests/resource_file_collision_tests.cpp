#include "ui/qt/resource_file_store.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

void check(bool ok) { if (!ok) throw std::runtime_error("collision regression failed"); }
void write(const QString& path, const QByteArray& bytes) {
  check(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile f(path); check(f.open(QIODevice::WriteOnly)); check(f.write(bytes)==bytes.size());
}
QByteArray read(const QString& path) {
  QFile f(path); check(f.open(QIODevice::ReadOnly)); return f.readAll();
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir tmp; check(tmp.isValid());
    const auto root=tmp.path()+"/resources";
    const auto configured=root+"/Verification/default.rsa";
    write(configured,"DEFAULT");
    const auto driver=tmp.path()+"/driver/signature.rsa";
    const auto application=tmp.path()+"/app/signature.rsa";
    write(driver,"DRIVER"); write(application,"APP");
    using uds::ui::qt::replaceConfiguredResourceFile;
    const auto a=replaceConfiguredResourceFile(driver,configured,root);
    const auto b=replaceConfiguredResourceFile(application,configured,root);
    check(a.success && b.success && a.stored_path!=b.stored_path);
    check(read(a.stored_path)=="DRIVER" && read(b.stored_path)=="APP");
    check(QFileInfo(b.stored_path).fileName()=="signature.rsa");
    const auto c=replaceConfiguredResourceFile(application,configured,root);
    check(c.success && c.stored_path!=a.stored_path && c.stored_path!=b.stored_path);
    check(read(a.stored_path)=="DRIVER" && read(b.stored_path)=="APP");
    // Selecting the existing managed file must not rewrite it.
    const auto same=replaceConfiguredResourceFile(a.stored_path,configured,root);
    check(same.success && same.stored_path==a.stored_path);
    const auto incomingDefault=tmp.path()+"/app/default.rsa";
    write(incomingDefault,"NEW DEFAULT NAME");
    const auto d=replaceConfiguredResourceFile(incomingDefault,configured,root);
    check(d.success && d.stored_path!=configured && read(configured)=="DEFAULT");
    check(read(driver)=="DRIVER" && read(application)=="APP");
    const auto missing=replaceConfiguredResourceFile(tmp.path()+"/missing",configured,root);
    check(!missing.success && read(a.stored_path)=="DRIVER");
    std::cout << "PASS: independent same-name selections, repeated imports, default preservation, source preservation, missing source\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
