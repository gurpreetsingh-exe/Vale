package net.verdagon.vale.scout

import net.verdagon.vale.parser._
import net.verdagon.vale.scout.patterns.{AbstractSP, AtomSP, CaptureS}
import net.verdagon.vale.scout.rules._
import net.verdagon.vale.{Err, Ok, vassert, vfail}
import org.scalatest.{FunSuite, Matchers}

class ScoutErrorHumanizerTests extends FunSuite with Matchers {
  private def compile(code: String): ProgramS = {
    Parser.runParser(code) match {
      case ParseFailure(err) => fail(err.toString)
      case ParseSuccess(program0) => {
        Scout.scoutProgram(List(program0)) match {
          case Err(e) => vfail(e.toString)
          case Ok(t) => t
        }
      }
    }
  }

  private def compileForError(code: String): ICompileErrorS = {
    Parser.runParser(code) match {
      case ParseFailure(err) => fail(err.toString)
      case ParseSuccess(program0) => {
        Scout.scoutProgram(List(program0)) match {
          case Err(e) => e
          case Ok(t) => vfail("Successfully compiled!\n" + t.toString)
        }
      }
    }
  }

  test("Humanize errors") {
    val filenamesAndSources = List(("file.vale", "blah blah blah\nblah blah blah"))

    vassert(ScoutErrorHumanizer.humanize(filenamesAndSources,
      VariableNameAlreadyExists(RangeS.testZero, CodeVarNameS("Spaceship")))
      .nonEmpty)
    vassert(ScoutErrorHumanizer.humanize(filenamesAndSources,
      InterfaceMethodNeedsSelf(RangeS.testZero))
      .nonEmpty)
    vassert(ScoutErrorHumanizer.humanize(filenamesAndSources,
      ForgotSetKeywordError(RangeS.testZero))
      .nonEmpty)
  }
}
