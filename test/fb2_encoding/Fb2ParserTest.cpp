#include <HalStorage.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "Fb2.h"

namespace {
class Fb2ParserTest : public testing::Test {
 protected:
  std::filesystem::path root;

  void SetUp() override {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() / ("crosspoint-fb2-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root / "books");
    Storage.setRoot(root);
  }

  void TearDown() override { std::filesystem::remove_all(root); }

  void writeBook(const std::string& bytes) {
    std::ofstream output(root / "books" / "book.fb2", std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  std::string readLogical(const std::string& path) {
    std::ifstream input(Storage.resolve(path.c_str()), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
};
}  // namespace

TEST_F(Fb2ParserTest, BuildsStructuredPackageFromMainAndNotesBodies) {
  writeBook(R"FB2(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description><title-info><book-title>Тестовая книга</book-title>
    <author><first-name>Иван</first-name><middle-name>Иванович</middle-name><last-name>Автор</last-name></author>
  </title-info></description>
  <body><section><title><p>Глава 1</p></title><p>Первый <emphasis>абзац</emphasis>.</p>
    <empty-line/><p>Второй абзац &amp; финал.</p></section></body>
  <body name="notes"><section><p>Скрытая сноска</p></section></body>
  <binary id="cover">VGhpcyBpcyBub3QgYm9vayB0ZXh0</binary>
</FictionBook>)FB2");

  Fb2 book("/books/book.fb2", "/.crosspoint");
  ASSERT_TRUE(book.load());
  EXPECT_EQ(book.getTitle(), "Тестовая книга");
  EXPECT_EQ(book.getAuthor(), "Иван Иванович Автор");
  EXPECT_EQ(book.getChapterCount(), 2);

  const std::string chapter = readLogical(book.getPackagePath() + "/OEBPS/text/chapter_0.xhtml");
  EXPECT_NE(chapter.find("<h1>"), std::string::npos);
  EXPECT_NE(chapter.find("Глава 1"), std::string::npos);
  EXPECT_NE(chapter.find("Первый <em>абзац</em>"), std::string::npos);
  EXPECT_NE(chapter.find("Второй абзац &amp; финал."), std::string::npos);

  const std::string notes = readLogical(book.getPackagePath() + "/OEBPS/text/chapter_1.xhtml");
  EXPECT_NE(notes.find("Скрытая сноска"), std::string::npos);
  EXPECT_EQ(chapter.find("VGhpcy"), std::string::npos);

  const std::string opf = readLogical(book.getPackagePath() + "/OEBPS/content.opf");
  EXPECT_NE(opf.find("chapter_0.xhtml"), std::string::npos);
  EXPECT_NE(opf.find("chapter_1.xhtml"), std::string::npos);
  EXPECT_NE(readLogical(book.getPackagePath() + "/OEBPS/toc.ncx").find("Глава 1"), std::string::npos);
}

TEST_F(Fb2ParserTest, ConvertsWindows1251ToUtf8) {
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"windows-1251\"?>"
      "<FictionBook><description><title-info><book-title>";
  xml += std::string("\xD2\xE5\xF1\xF2", 4);  // Тест
  xml += "</book-title><author><first-name>";
  xml += std::string("\xC8\xE2\xE0\xED", 4);  // Иван
  xml += "</first-name></author></title-info></description><body><section><p>";
  xml += std::string("\xCF\xF0\xE8\xE2\xE5\xF2", 6);  // Привет
  xml += "</p></section></body></FictionBook>";
  writeBook(xml);

  Fb2 book("/books/book.fb2", "/.crosspoint");
  ASSERT_TRUE(book.load());
  EXPECT_EQ(book.getTitle(), "Тест");
  EXPECT_EQ(book.getAuthor(), "Иван");
  EXPECT_NE(readLogical(book.getPackagePath() + "/OEBPS/text/chapter_0.xhtml").find("Привет"), std::string::npos);
}

TEST_F(Fb2ParserTest, RejectsMalformedXmlWithoutPublishingCache) {
  writeBook("<?xml version=\"1.0\"?><FictionBook><body><section><p>broken");
  Fb2 book("/books/book.fb2", "/.crosspoint");
  EXPECT_FALSE(book.load());
  EXPECT_FALSE(Storage.exists((book.getPackagePath() + "/META-INF/container.xml").c_str()));
}

TEST_F(Fb2ParserTest, PreservesCoverImagesAndCrossChapterFootnotes) {
  writeBook(R"FB2(<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns:l="http://www.w3.org/1999/xlink">
  <description><title-info><book-title>Книга с картинкой</book-title><lang>ru</lang>
    <coverpage><image l:href="#cover"/></coverpage></title-info></description>
  <body><section id="main"><title><p>Текст</p></title>
    <p>Смотри <a l:href="#note-1">[1]</a>.</p><image l:href="#cover"/>
  </section></body>
  <body name="notes"><section id="note-1"><title><p>Примечание</p></title><p>Текст сноски</p></section></body>
  <binary id="cover" content-type="image/png">iVBORw0KGgo=</binary>
</FictionBook>)FB2");

  Fb2 book("/books/book.fb2", "/.crosspoint");
  ASSERT_TRUE(book.load());
  EXPECT_EQ(book.getLanguage(), "ru");

  const std::string main = readLogical(book.getPackagePath() + "/OEBPS/text/chapter_0.xhtml");
  EXPECT_NE(main.find("href=\"chapter_1.xhtml#fb2-"), std::string::npos);
  EXPECT_NE(main.find("src=\"../images/image_0.png\""), std::string::npos);

  const std::string opf = readLogical(book.getPackagePath() + "/OEBPS/content.opf");
  EXPECT_NE(opf.find("content=\"cover-image\""), std::string::npos);
  EXPECT_NE(opf.find("media-type=\"image/png\""), std::string::npos);

  const std::string image = readLogical(book.getPackagePath() + "/OEBPS/images/image_0.png");
  ASSERT_GE(image.size(), 8u);
  EXPECT_EQ(static_cast<unsigned char>(image[0]), 0x89);
  EXPECT_EQ(image.substr(1, 3), "PNG");
}

TEST_F(Fb2ParserTest, SplitsOversizedSingleSectionAtParagraphBoundary) {
  const std::string longParagraph(50 * 1024, 'A');
  writeBook(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><FictionBook><description><title-info>"
      "<book-title>Большая секция</book-title></title-info></description><body><section><title><p>Глава</p>"
      "</title><p>" +
      longParagraph + "</p><p>Следующий абзац</p></section></body></FictionBook>");

  Fb2 book("/books/book.fb2", "/.crosspoint");
  ASSERT_TRUE(book.load());
  EXPECT_EQ(book.getChapterCount(), 2);
  EXPECT_NE(readLogical(book.getPackagePath() + "/OEBPS/text/chapter_0.xhtml").find(longParagraph), std::string::npos);
  const std::string continuation = readLogical(book.getPackagePath() + "/OEBPS/text/chapter_1.xhtml");
  EXPECT_NE(continuation.find("class=\"continuation\""), std::string::npos);
  EXPECT_NE(continuation.find("Следующий абзац"), std::string::npos);
}
