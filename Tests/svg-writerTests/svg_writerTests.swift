import XCTest
@testable import svg_writer

final class svg_writerTests: XCTestCase {
    func testExample() throws {
        // This is an example of a functional test case.
        // Use XCTAssert and related functions to verify your tests produce the correct
        // results.
        XCTAssertEqual(svg_writer().text, "Hello, World!")
    }
}
