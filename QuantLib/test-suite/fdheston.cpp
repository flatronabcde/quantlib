/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
  Copyright (C) 2008, 2009, 2014 Klaus Spanderen
  Copyright (C) 2014 Johannes Göttker-Schnetmann

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "fdheston.hpp"
#include "utilities.hpp"

#include <ql/quotes/simplequote.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/instruments/dividendvanillaoption.hpp>
#include <ql/utilities/steppingiterator.hpp>
#include <ql/math/incompletegamma.hpp>
#include <ql/math/functional.hpp>
#include <ql/math/solvers1d/brent.hpp>
#include <ql/math/distributions/gammadistribution.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/bicubicsplineinterpolation.hpp>
#include <ql/math/interpolations/bilinearinterpolation.hpp>
#include <ql/math/integrals/gausslobattointegral.hpp>
#include <ql/math/integrals/discreteintegrals.hpp>
#include <ql/models/equity/hestonmodel.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancesurface.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/vanilla/analytichestonengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/barrier/fdhestonbarrierengine.hpp>
#include <ql/pricingengines/vanilla/fdhestonvanillaengine.hpp>
#include <ql/pricingengines/barrier/fdblackscholesbarrierengine.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>
#include <ql/methods/finitedifferences/meshers/fdmmesher.hpp>
#include <ql/methods/finitedifferences/meshers/fdmmeshercomposite.hpp>
#include <ql/methods/finitedifferences/meshers/fdmblackscholesmesher.hpp>
#include <ql/methods/finitedifferences/meshers/predefined1dmesher.hpp>
#include <ql/methods/finitedifferences/meshers/uniform1dmesher.hpp>
#include <ql/methods/finitedifferences/meshers/concentrating1dmesher.hpp>
#include <ql/methods/finitedifferences/schemes/douglasscheme.hpp>
#include <ql/methods/finitedifferences/schemes/hundsdorferscheme.hpp>
#include <ql/methods/finitedifferences/solvers/fdmbackwardsolver.hpp>
#include <ql/methods/finitedifferences/utilities/fdmmesherintegral.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearoplayout.hpp>
#include <ql/experimental/finitedifferences/fdmhestonfwdop.hpp>
#include <ql/experimental/finitedifferences/fdmsquarerootfwdop.hpp>
#include <ql/experimental/finitedifferences/fdmblackscholesfwdop.hpp>
#include <ql/experimental/finitedifferences/fdmhestongreensfct.hpp>
#include <ql/experimental/exoticoptions/analyticpdfhestonengine.hpp>

#include <boost/assign/std/vector.hpp>
#include <boost/math/special_functions/gamma.hpp>

#include <boost/bind.hpp>

using namespace QuantLib;
using namespace boost::assign;
using boost::unit_test_framework::test_suite;

namespace {
    struct NewBarrierOptionData {
        Barrier::Type barrierType;
        Real barrier;
        Real rebate;
        Option::Type type;
        Real strike;
        Real s;        // spot
        Rate q;        // dividend
        Rate r;        // risk-free rate
        Time t;        // time to maturity
        Volatility v;  // volatility
    };
}

void FdHestonTest::testFdmHestonBarrierVsBlackScholes() {

    BOOST_TEST_MESSAGE("Testing FDM with barrier option in Heston model...");

    SavedSettings backup;

    NewBarrierOptionData values[] = {
        /* The data below are from
          "Option pricing formulas", E.G. Haug, McGraw-Hill 1998 pag. 72
        */
        //     barrierType, barrier, rebate,         type, strike,     s,    q,    r,    t,    v
        { Barrier::DownOut,    95.0,    3.0, Option::Call,     90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,    95.0,    3.0, Option::Call,    100, 100.0, 0.00, 0.08, 1.00, 0.30},
        { Barrier::DownOut,    95.0,    3.0, Option::Call,    110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,   100.0,    3.0, Option::Call,     90, 100.0, 0.00, 0.08, 0.25, 0.25},
        { Barrier::DownOut,   100.0,    3.0, Option::Call,    100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,   100.0,    3.0, Option::Call,    110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpOut,     105.0,    3.0, Option::Call,     90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpOut,     105.0,    3.0, Option::Call,    100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpOut,     105.0,    3.0, Option::Call,    110, 100.0, 0.04, 0.08, 0.50, 0.25},

        { Barrier::DownIn,     95.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,     95.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,     95.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,    100.0,    3.0, Option::Call,    90, 100.0, 0.00, 0.08, 0.25, 0.25},
        { Barrier::DownIn,    100.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,    100.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpIn,      105.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpIn,      105.0,    3.0, Option::Call,   100, 100.0, 0.00, 0.08, 0.40, 0.25},
        { Barrier::UpIn,      105.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.15},

        { Barrier::DownOut,    95.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,    95.0,    3.0, Option::Call,   100, 100.0, 0.00, 0.08, 0.40, 0.35},
        { Barrier::DownOut,    95.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,   100.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.15},
        { Barrier::DownOut,   100.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,   100.0,    3.0, Option::Call,   110, 100.0, 0.00, 0.00, 1.00, 0.20},
        { Barrier::UpOut,     105.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpOut,     105.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpOut,     105.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.30},

        { Barrier::DownIn,     95.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,     95.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,     95.0,    3.0, Option::Call,   110, 100.0, 0.00, 0.08, 1.00, 0.30},
        { Barrier::DownIn,    100.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,    100.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,    100.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpIn,      105.0,    3.0, Option::Call,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpIn,      105.0,    3.0, Option::Call,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpIn,      105.0,    3.0, Option::Call,   110, 100.0, 0.04, 0.08, 0.50, 0.30},

        { Barrier::DownOut,    95.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,    95.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,    95.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,   100.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,   100.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownOut,   100.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpOut,     105.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpOut,     105.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpOut,     105.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.25},

        { Barrier::DownIn,     95.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,     95.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,     95.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,    100.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,    100.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::DownIn,    100.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpIn,      105.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpIn,      105.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.25},
        { Barrier::UpIn,      105.0,    3.0,  Option::Put,   110, 100.0, 0.00, 0.04, 1.00, 0.15},

        { Barrier::DownOut,    95.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,    95.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,    95.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,   100.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,   100.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownOut,   100.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpOut,     105.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpOut,     105.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpOut,     105.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.30},

        { Barrier::DownIn,     95.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,     95.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,     95.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,    100.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,    100.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::DownIn,    100.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 1.00, 0.15},
        { Barrier::UpIn,      105.0,    3.0,  Option::Put,    90, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpIn,      105.0,    3.0,  Option::Put,   100, 100.0, 0.04, 0.08, 0.50, 0.30},
        { Barrier::UpIn,      105.0,    3.0,  Option::Put,   110, 100.0, 0.04, 0.08, 0.50, 0.30}
    };
    
    const DayCounter dc = Actual365Fixed();     
    const Date todaysDate(28, March, 2004);
    const Date exerciseDate(28, March, 2005);
    Settings::instance().evaluationDate() = todaysDate;

    Handle<Quote> spot(
            boost::shared_ptr<Quote>(new SimpleQuote(0.0)));
    boost::shared_ptr<SimpleQuote> qRate(new SimpleQuote(0.0));
    Handle<YieldTermStructure> qTS(flatRate(qRate, dc));
    boost::shared_ptr<SimpleQuote> rRate(new SimpleQuote(0.0));
    Handle<YieldTermStructure> rTS(flatRate(rRate, dc));
    boost::shared_ptr<SimpleQuote> vol(new SimpleQuote(0.0));
    Handle<BlackVolTermStructure> volTS(flatVol(vol, dc));

    boost::shared_ptr<BlackScholesMertonProcess> bsProcess(
                      new BlackScholesMertonProcess(spot, qTS, rTS, volTS));

    boost::shared_ptr<PricingEngine> analyticEngine(
                                        new AnalyticBarrierEngine(bsProcess));
    
    for (Size i=0; i<LENGTH(values); i++) {
        Date exDate = todaysDate + Integer(values[i].t*365+0.5);
        boost::shared_ptr<Exercise> exercise(new EuropeanExercise(exDate));

        boost::dynamic_pointer_cast<SimpleQuote>(spot .currentLink())
                                                    ->setValue(values[i].s);
        qRate->setValue(values[i].q);
        rRate->setValue(values[i].r);
        vol  ->setValue(values[i].v);

        boost::shared_ptr<StrikedTypePayoff> payoff(new
                    PlainVanillaPayoff(values[i].type, values[i].strike));

        BarrierOption barrierOption(values[i].barrierType, values[i].barrier,
                                    values[i].rebate, payoff, exercise);

        const Real v0 = vol->value()*vol->value();
        boost::shared_ptr<HestonProcess> hestonProcess(
             new HestonProcess(rTS, qTS, spot, v0, 1.0, v0, 0.00001, 0.0));

        barrierOption.setPricingEngine(boost::shared_ptr<PricingEngine>(
            new FdHestonBarrierEngine(boost::shared_ptr<HestonModel>(
                              new HestonModel(hestonProcess)), 200, 400, 3)));

        const Real calculatedHE = barrierOption.NPV();
    
        barrierOption.setPricingEngine(analyticEngine);
        const Real expected = barrierOption.NPV();
    
        const Real tol = 0.002;
        if (std::fabs(calculatedHE - expected)/expected > tol) {
            BOOST_ERROR("Failed to reproduce expected Heston npv"
                        << "\n    calculated: " << calculatedHE
                        << "\n    expected:   " << expected
                        << "\n    tolerance:  " << tol); 
        }
    }
}

void FdHestonTest::testFdmHestonBarrier() {

    BOOST_TEST_MESSAGE("Testing FDM with barrier option for Heston model vs "
                       "Black-Scholes model...");

    SavedSettings backup;

    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

    Handle<YieldTermStructure> rTS(flatRate(0.05, Actual365Fixed()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual365Fixed()));

    boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, s0, 0.04, 2.5, 0.04, 0.66, -0.8));

    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(28, March, 2005);

    boost::shared_ptr<Exercise> exercise(new EuropeanExercise(exerciseDate));

    boost::shared_ptr<StrikedTypePayoff> payoff(new
                                      PlainVanillaPayoff(Option::Call, 100));

    BarrierOption barrierOption(Barrier::UpOut, 135, 0.0, payoff, exercise);

    barrierOption.setPricingEngine(boost::shared_ptr<PricingEngine>(
            new FdHestonBarrierEngine(boost::shared_ptr<HestonModel>(
                              new HestonModel(hestonProcess)), 50, 400, 100)));

    const Real tol = 0.01;
    const Real npvExpected   =  9.1530;
    const Real deltaExpected =  0.5218;
    const Real gammaExpected = -0.0354;

    if (std::fabs(barrierOption.NPV() - npvExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected npv"
                    << "\n    calculated: " << barrierOption.NPV()
                    << "\n    expected:   " << npvExpected
                    << "\n    tolerance:  " << tol); 
    }
    if (std::fabs(barrierOption.delta() - deltaExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected delta"
                    << "\n    calculated: " << barrierOption.delta()
                    << "\n    expected:   " << deltaExpected
                    << "\n    tolerance:  " << tol); 
    }
    if (std::fabs(barrierOption.gamma() - gammaExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected gamma"
                    << "\n    calculated: " << barrierOption.gamma()
                    << "\n    expected:   " << gammaExpected
                    << "\n    tolerance:  " << tol); 
    }
}

void FdHestonTest::testFdmHestonAmerican() {

    BOOST_TEST_MESSAGE("Testing FDM with American option in Heston model...");

    SavedSettings backup;

    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

    Handle<YieldTermStructure> rTS(flatRate(0.05, Actual365Fixed()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual365Fixed()));

    boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, s0, 0.04, 2.5, 0.04, 0.66, -0.8));

    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(28, March, 2005);

    boost::shared_ptr<Exercise> exercise(new AmericanExercise(exerciseDate));

    boost::shared_ptr<StrikedTypePayoff> payoff(new
                                      PlainVanillaPayoff(Option::Put, 100));

    VanillaOption option(payoff, exercise);
    boost::shared_ptr<PricingEngine> engine(
         new FdHestonVanillaEngine(boost::shared_ptr<HestonModel>(
                             new HestonModel(hestonProcess)), 200, 100, 50));
    option.setPricingEngine(engine);
    
    const Real tol = 0.01;
    const Real npvExpected   =  5.66032;
    const Real deltaExpected = -0.30065;
    const Real gammaExpected =  0.02202;
    
    if (std::fabs(option.NPV() - npvExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected npv"
                    << "\n    calculated: " << option.NPV()
                    << "\n    expected:   " << npvExpected
                    << "\n    tolerance:  " << tol); 
    }
    if (std::fabs(option.delta() - deltaExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected delta"
                    << "\n    calculated: " << option.delta()
                    << "\n    expected:   " << deltaExpected
                    << "\n    tolerance:  " << tol); 
    }
    if (std::fabs(option.gamma() - gammaExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected gamma"
                    << "\n    calculated: " << option.gamma()
                    << "\n    expected:   " << gammaExpected
                    << "\n    tolerance:  " << tol); 
    }
}


void FdHestonTest::testFdmHestonIkonenToivanen() {

    BOOST_TEST_MESSAGE("Testing FDM Heston for Ikonen and Toivanen tests...");

    /* check prices of american puts as given in:
       From Efficient numerical methods for pricing American options under 
       stochastic volatility, Samuli Ikonen, Jari Toivanen, 
       http://users.jyu.fi/~tene/papers/reportB12-05.pdf
    */
    SavedSettings backup;

    Handle<YieldTermStructure> rTS(flatRate(0.10, Actual360()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual360()));

    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(26, June, 2004);

    boost::shared_ptr<Exercise> exercise(new AmericanExercise(exerciseDate));

    boost::shared_ptr<StrikedTypePayoff> payoff(new
                                      PlainVanillaPayoff(Option::Put, 10));

    VanillaOption option(payoff, exercise);

    Real strikes[]  = { 8, 9, 10, 11, 12 };
    Real expected[] = { 2.00000, 1.10763, 0.520038, 0.213681, 0.082046 };
    const Real tol = 0.001;
    
    for (Size i=0; i < LENGTH(strikes); ++i) {
        Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(strikes[i])));
        boost::shared_ptr<HestonProcess> hestonProcess(
            new HestonProcess(rTS, qTS, s0, 0.0625, 5, 0.16, 0.9, 0.1));
    
        boost::shared_ptr<PricingEngine> engine(
             new FdHestonVanillaEngine(boost::shared_ptr<HestonModel>(
                                 new HestonModel(hestonProcess)), 100, 400));
        option.setPricingEngine(engine);
        
        Real calculated = option.NPV();
        if (std::fabs(calculated - expected[i]) > tol) {
            BOOST_ERROR("Failed to reproduce expected npv"
                        << "\n    strike:     " << strikes[i]
                        << "\n    calculated: " << calculated
                        << "\n    expected:   " << expected[i]
                        << "\n    tolerance:  " << tol); 
        }
    }
}

void FdHestonTest::testFdmHestonBlackScholes() {

    BOOST_TEST_MESSAGE("Testing FDM Heston with Black Scholes model...");

    SavedSettings backup;


    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(26, June, 2004);

    Handle<YieldTermStructure> rTS(flatRate(0.10, Actual360()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual360()));
    Handle<BlackVolTermStructure> volTS(
                    flatVol(rTS->referenceDate(), 0.25, rTS->dayCounter()));
    
    boost::shared_ptr<Exercise> exercise(new EuropeanExercise(exerciseDate));

    boost::shared_ptr<StrikedTypePayoff> payoff(new
                                      PlainVanillaPayoff(Option::Put, 10));

    VanillaOption option(payoff, exercise);

    Real strikes[]  = { 8, 9, 10, 11, 12 };
    const Real tol = 0.0001;
    
    for (Size i=0; i < LENGTH(strikes); ++i) {
        Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(strikes[i])));

        boost::shared_ptr<GeneralizedBlackScholesProcess> bsProcess(
                       new GeneralizedBlackScholesProcess(s0, qTS, rTS, volTS));

        option.setPricingEngine(boost::shared_ptr<PricingEngine>(
                                        new AnalyticEuropeanEngine(bsProcess)));
        
        const Real expected = option.NPV();
        
        boost::shared_ptr<HestonProcess> hestonProcess(
            new HestonProcess(rTS, qTS, s0, 0.0625, 1, 0.0625, 0.0001, 0.0));

        // Hundsdorfer scheme
        option.setPricingEngine(boost::shared_ptr<PricingEngine>(
             new FdHestonVanillaEngine(boost::shared_ptr<HestonModel>(
                                           new HestonModel(hestonProcess)), 
                                       100, 400)));
        
        Real calculated = option.NPV();
        if (std::fabs(calculated - expected) > tol) {
            BOOST_ERROR("Failed to reproduce expected npv"
                        << "\n    strike:     " << strikes[i]
                        << "\n    calculated: " << calculated
                        << "\n    expected:   " << expected
                        << "\n    tolerance:  " << tol); 
        }
        
        // Explicit scheme
        option.setPricingEngine(boost::shared_ptr<PricingEngine>(
             new FdHestonVanillaEngine(boost::shared_ptr<HestonModel>(
                                           new HestonModel(hestonProcess)), 
                                       10000, 400, 5, 0, 
                                       FdmSchemeDesc::ExplicitEuler())));
        
        calculated = option.NPV();
        if (std::fabs(calculated - expected) > tol) {
            BOOST_ERROR("Failed to reproduce expected npv"
                        << "\n    strike:     " << strikes[i]
                        << "\n    calculated: " << calculated
                        << "\n    expected:   " << expected
                        << "\n    tolerance:  " << tol); 
        }
    }
}



void FdHestonTest::testFdmHestonEuropeanWithDividends() {

    BOOST_TEST_MESSAGE("Testing FDM with European option with dividends"
                       " in Heston model...");

    SavedSettings backup;

    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(100.0)));

    Handle<YieldTermStructure> rTS(flatRate(0.05, Actual365Fixed()));
    Handle<YieldTermStructure> qTS(flatRate(0.0 , Actual365Fixed()));

    boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, s0, 0.04, 2.5, 0.04, 0.66, -0.8));

    Settings::instance().evaluationDate() = Date(28, March, 2004);
    Date exerciseDate(28, March, 2005);

    boost::shared_ptr<Exercise> exercise(new AmericanExercise(exerciseDate));

    boost::shared_ptr<StrikedTypePayoff> payoff(new
                                      PlainVanillaPayoff(Option::Put, 100));

    const std::vector<Real> dividends(1, 5);
    const std::vector<Date> dividendDates(1, Date(28, September, 2004));

    DividendVanillaOption option(payoff, exercise, dividendDates, dividends);
    boost::shared_ptr<PricingEngine> engine(
         new FdHestonVanillaEngine(boost::shared_ptr<HestonModel>(
                             new HestonModel(hestonProcess)), 50, 100, 50));
    option.setPricingEngine(engine);
    
    const Real tol = 0.01;
    const Real gammaTol = 0.001;
    const Real npvExpected   =  7.365075;
    const Real deltaExpected = -0.396678;
    const Real gammaExpected =  0.027681;
        
    if (std::fabs(option.NPV() - npvExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected npv"
                    << "\n    calculated: " << option.NPV()
                    << "\n    expected:   " << npvExpected
                    << "\n    tolerance:  " << tol); 
    }
    if (std::fabs(option.delta() - deltaExpected) > tol) {
        BOOST_ERROR("Failed to reproduce expected delta"
                    << "\n    calculated: " << option.delta()
                    << "\n    expected:   " << deltaExpected
                    << "\n    tolerance:  " << tol); 
    }
    if (std::fabs(option.gamma() - gammaExpected) > gammaTol) {
        BOOST_ERROR("Failed to reproduce expected gamma"
                    << "\n    calculated: " << option.gamma()
                    << "\n    expected:   " << gammaExpected
                    << "\n    tolerance:  " << tol); 
    }
}

namespace {
    struct HestonTestData {
        Real kappa;
        Real theta;
        Real sigma;
        Real rho;
        Real r;
        Real q;
        Real T;
        Real K;
    };    
}

void FdHestonTest::testFdmHestonConvergence() {

    /* convergence tests based on 
       ADI finite difference schemes for option pricing in the
       Heston model with correlation, K.J. in t'Hout and S. Foulon
    */
    
    BOOST_TEST_MESSAGE("Testing FDM Heston convergence...");

    SavedSettings backup;
    
    HestonTestData values[] = {
        { 1.5   , 0.04  , 0.3   , -0.9   , 0.025 , 0.0   , 1.0 , 100 },
        { 3.0   , 0.12  , 0.04  , 0.6    , 0.01  , 0.04  , 1.0 , 100 },
        { 0.6067, 0.0707, 0.2928, -0.7571, 0.03  , 0.0   , 3.0 , 100 },
        { 2.5   , 0.06  , 0.5   , -0.1   , 0.0507, 0.0469, 0.25, 100 }
    };

    FdmSchemeDesc schemes[] = { FdmSchemeDesc::Hundsdorfer(), 
                                FdmSchemeDesc::ModifiedCraigSneyd(),
                                FdmSchemeDesc::ModifiedHundsdorfer(), 
                                FdmSchemeDesc::CraigSneyd() };
    
    Size tn[] = { 100 };
    Real v0[] = { 0.04 };
    
    const Date todaysDate(28, March, 2004); 
    Settings::instance().evaluationDate() = todaysDate;
    
    Handle<Quote> s0(boost::shared_ptr<Quote>(new SimpleQuote(75.0)));

    for (Size l=0; l < LENGTH(schemes); ++l) {
        for (Size i=0; i < LENGTH(values); ++i) {
            for (Size j=0; j < LENGTH(tn); ++j) {
                for (Size k=0; k < LENGTH(v0); ++k) {
                    Handle<YieldTermStructure> rTS(
                        flatRate(values[i].r, Actual365Fixed()));
                    Handle<YieldTermStructure> qTS(
                        flatRate(values[i].q, Actual365Fixed()));
                
                    boost::shared_ptr<HestonProcess> hestonProcess(
                        new HestonProcess(rTS, qTS, s0, 
                                          v0[k], 
                                          values[i].kappa, 
                                          values[i].theta, 
                                          values[i].sigma, 
                                          values[i].rho));
                
                    Date exerciseDate = todaysDate 
                        + Period(static_cast<Integer>(values[i].T*365), Days);
                    boost::shared_ptr<Exercise> exercise(
                                          new EuropeanExercise(exerciseDate));
                
                    boost::shared_ptr<StrikedTypePayoff> payoff(new
                               PlainVanillaPayoff(Option::Call, values[i].K));
            
                    VanillaOption option(payoff, exercise);
                    boost::shared_ptr<PricingEngine> engine(
                         new FdHestonVanillaEngine(
                             boost::shared_ptr<HestonModel>(
                                 new HestonModel(hestonProcess)), 
                             tn[j], 400, 100, 0, 
                             schemes[l]));
                    option.setPricingEngine(engine);
                    
                    const Real calculated = option.NPV();
                    
                    boost::shared_ptr<PricingEngine> analyticEngine(
                        new AnalyticHestonEngine(
                            boost::shared_ptr<HestonModel>(
                                new HestonModel(hestonProcess)), 144));
                    
                    option.setPricingEngine(analyticEngine);
                    const Real expected = option.NPV();
                    if (   std::fabs(expected - calculated)/expected > 0.02
                        && std::fabs(expected - calculated) > 0.002) {
                        BOOST_ERROR("Failed to reproduce expected npv"
                                    << "\n    calculated: " << calculated
                                    << "\n    expected:   " << expected
                                    << "\n    tolerance:  " << 0.01); 
                    }
                }
            }
        }
    }
}

namespace {
    Real fokkerPlanckPrice1D(const boost::shared_ptr<FdmMesher>& mesher,
                             const boost::shared_ptr<FdmLinearOpComposite>& op,
                             const boost::shared_ptr<StrikedTypePayoff>& payoff,
                             Real x0, Time maturity, Size tGrid) {

        const Array x = mesher->locations(0);
        Array p(x.size(), 0.0);

        QL_REQUIRE(x.size() > 3 && x[1] <= x0 && x[x.size()-2] >= x0,
                   "insufficient mesher");

        const Array::const_iterator upperb
            = std::upper_bound(x.begin(), x.end(), x0);
        const Array::const_iterator lowerb = upperb-1;

        if (close_enough(*upperb, x0)) {
            const Size idx = std::distance(x.begin(), upperb);
            const Real dx = (x[idx+1]-x[idx-1])/2.0;
            p[idx] = 1.0/dx;
        }
        else if (close_enough(*lowerb, x0)) {
            const Size idx = std::distance(x.begin(), lowerb);
            const Real dx = (x[idx+1]-x[idx-1])/2.0;
            p[idx] = 1.0/dx;
        } else {
            const Real dx = *upperb - *lowerb;
            const Real lowerP = (*upperb - x0)/dx;
            const Real upperP = (x0 - *lowerb)/dx;

            const Size lowerIdx = std::distance(x.begin(), lowerb);
            const Size upperIdx = std::distance(x.begin(), upperb);

            const Real lowerDx = (x[lowerIdx+1]-x[lowerIdx-1])/2.0;
            const Real upperDx = (x[upperIdx+1]-x[upperIdx-1])/2.0;

            p[lowerIdx] = lowerP/lowerDx;
            p[upperIdx] = upperP/upperDx;
        }

        DouglasScheme evolver(FdmSchemeDesc::Douglas().theta, op);
        const Time dt = maturity/tGrid;
        evolver.setStep(dt);

        for (Time t=dt; t <= maturity+20*QL_EPSILON; t+=dt) {
            evolver.step(p, t);
        }

        Array payoffTimesDensity(x.size());
        for (Size i=0; i < x.size(); ++i) {
            payoffTimesDensity[i] = payoff->operator()(std::exp(x[i]))*p[i];
        }

        CubicNaturalSpline f(x.begin(), x.end(), payoffTimesDensity.begin());
        f.enableExtrapolation();
        return GaussLobattoIntegral(1000, 1e-6)(f, x.front(), x.back());
    }
}

void FdHestonTest::testBlackScholesFokkerPlanckFwdEquation() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation for BS process...");

    SavedSettings backup;

    const DayCounter dc = ActualActual();
    const Date todaysDate = Date(28, Dec, 2012);
    Settings::instance().evaluationDate() = todaysDate;

    const Date maturityDate = todaysDate + Period(2, Years);
    const Time maturity = dc.yearFraction(todaysDate, maturityDate);

    const Real s0 = 100;
    const Real x0 = std::log(s0);
    const Rate r = 0.035;
    const Rate q = 0.01;
    const Volatility v = 0.35;

    const Size xGrid = 2*100+1;
    const Size tGrid = 400;

    const Handle<Quote> spot(boost::shared_ptr<Quote>(new SimpleQuote(s0)));
    const Handle<YieldTermStructure> qTS(flatRate(q, dc));
    const Handle<YieldTermStructure> rTS(flatRate(r, dc));
    const Handle<BlackVolTermStructure> vTS(flatVol(v, dc));

    const boost::shared_ptr<GeneralizedBlackScholesProcess> process(
        new GeneralizedBlackScholesProcess(spot, qTS, rTS, vTS));

    const boost::shared_ptr<PricingEngine> engine(
        new AnalyticEuropeanEngine(process));

    const boost::shared_ptr<FdmMesher> uniformMesher(
        new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
            new FdmBlackScholesMesher(xGrid, process, maturity, s0))));

    const boost::shared_ptr<FdmLinearOpComposite> uniformBSFwdOp(
        new FdmBlackScholesFwdOp(uniformMesher, process, s0, 0));

    const boost::shared_ptr<FdmMesher> concentratedMesher(
        new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
            new FdmBlackScholesMesher(xGrid, process, maturity, s0,
                                      Null<Real>(), Null<Real>(), 0.0001, 1.5,
                                      std::pair<Real, Real>(s0, 0.1)))));

    const boost::shared_ptr<FdmLinearOpComposite> concentratedBSFwdOp(
        new FdmBlackScholesFwdOp(concentratedMesher, process, s0, 0));

    const boost::shared_ptr<FdmMesher> shiftedMesher(
        new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
            new FdmBlackScholesMesher(xGrid, process, maturity, s0,
                                      Null<Real>(), Null<Real>(), 0.0001, 1.5,
                                      std::pair<Real, Real>(s0*1.1, 0.2)))));

    const boost::shared_ptr<FdmLinearOpComposite> shiftedBSFwdOp(
        new FdmBlackScholesFwdOp(shiftedMesher, process, s0, 0));

    const boost::shared_ptr<Exercise> exercise(
        new EuropeanExercise(maturityDate));
    const Real strikes[] = { 50, 80, 100, 130, 150 };

    for (Size i=0; i < LENGTH(strikes); ++i) {
        const boost::shared_ptr<StrikedTypePayoff> payoff(
            new PlainVanillaPayoff(Option::Call, strikes[i]));

        VanillaOption option(payoff, exercise);
        option.setPricingEngine(engine);

        const Real expected = option.NPV()/rTS->discount(maturityDate);
        const Real calcUniform
            = fokkerPlanckPrice1D(uniformMesher, uniformBSFwdOp,
                                  payoff, x0, maturity, tGrid);
        const Real calcConcentrated
            = fokkerPlanckPrice1D(concentratedMesher, concentratedBSFwdOp,
                                  payoff, x0, maturity, tGrid);
        const Real calcShifted
            = fokkerPlanckPrice1D(shiftedMesher, shiftedBSFwdOp,
                                  payoff, x0, maturity, tGrid);
        const Real tol = 0.02;

        if (std::fabs(expected - calcUniform) > tol) {
            BOOST_FAIL("failed to reproduce european option price "
                       << "with an uniform mesher"
                       << "\n   strike:     " << strikes[i]
                       << QL_FIXED << std::setprecision(8)
                       << "\n   calculated: " << calcUniform
                       << "\n   expected:   " << expected
                       << "\n   tolerance:  " << tol);
        }
        if (std::fabs(expected - calcConcentrated) > tol) {
            BOOST_FAIL("failed to reproduce european option price "
                       << "with a concentrated mesher"
                       << "\n   strike:     " << strikes[i]
                       << QL_FIXED << std::setprecision(8)
                       << "\n   calculated: " << calcConcentrated
                       << "\n   expected:   " << expected
                       << "\n   tolerance:  " << tol);
        }
        if (std::fabs(expected - calcShifted) > tol) {
            BOOST_FAIL("failed to reproduce european option price "
                       << "with a shifted mesher"
                       << "\n   strike:     " << strikes[i]
                       << QL_FIXED << std::setprecision(8)
                       << "\n   calculated: " << calcShifted
                       << "\n   expected:   " << expected
                       << "\n   tolerance:  " << tol);
        }
    }
}


namespace {
    Real stationaryProbabilityFct(Real kappa, Real theta,
                                   Real sigma, Real v) {
        const Real alpha = 2*kappa*theta/(sigma*sigma);
        const Real beta = alpha/theta;

        return std::pow(beta, alpha)*std::pow(v, alpha-1) //
                *std::exp(-beta*v-GammaFunction().logValue(alpha));
    }

	Real stationaryLogProbabilityFct(Real kappa, Real theta,
                                   Real sigma, Real z) {
        const Real alpha = 2*kappa*theta/(sigma*sigma);
        const Real beta = alpha/theta;

        return std::pow(beta, alpha)*std::exp(z*alpha) 
                *std::exp(-beta*std::exp(z)-GammaFunction().logValue(alpha));
    }

    class StationaryDistributionFct : public std::unary_function<Real,Real> {
      public:
        StationaryDistributionFct(Real kappa, Real theta, Real sigma)
        : kappa_(kappa), theta_(theta), sigma_(sigma) {}

        Real operator()(Real v) const {
            const Real alpha = 2*kappa_*theta_/(sigma_*sigma_);
            const Real beta = alpha/theta_;

            return boost::math::gamma_p(alpha, beta*v);
        }
      private:
        const Real kappa_, theta_, sigma_;
    };

    Real invStationaryDistributionFct(Real kappa, Real theta,
                                      Real sigma, Real q) {
        const Real alpha = 2*kappa*theta/(sigma*sigma);
        const Real beta = alpha/theta;

        return boost::math::gamma_p_inv(alpha, q)/beta;
    }
}

void FdHestonTest::testSquareRootZeroFlowBC() {
    BOOST_TEST_MESSAGE("Testing zero-flow BC for the square root process...");

    SavedSettings backup;

    const Real kappa = 1.0;
    const Real theta = 0.4;
    const Real sigma = 0.8;
    const Real v_0   = 0.1;
    const Time t     = 1.0;

    const Real vmin = 0.0005;
    const Real h    = 0.0001;

    const Real expected[5][5]
        = {{ 0.000548, -0.000245, -0.005657, -0.001167, -0.000024},
           {-0.000595, -0.000701, -0.003296, -0.000883, -0.000691},
           {-0.001277, -0.001320, -0.003128, -0.001399, -0.001318},
           {-0.001979, -0.002002, -0.003425, -0.002047, -0.002001},
           {-0.002715, -0.002730, -0.003920, -0.002760, -0.002730} };

    for (Size i=0; i < 5; ++i) {
        const Real v = vmin + i*0.001;
        const Real vm2 = v - 2*h;
        const Real vm1 = v - h;
        const Real v0  = v;
        const Real v1  = v + h;
        const Real v2  = v + 2*h;

        const Real pm2
        	= squareRootProcessGreensFct(v_0, kappa, theta, sigma, t, vm2);
        const Real pm1
        	= squareRootProcessGreensFct(v_0, kappa, theta, sigma, t, vm1);
        const Real p0
        	= squareRootProcessGreensFct(v_0, kappa, theta, sigma, t, v0);
        const Real p1
        	= squareRootProcessGreensFct(v_0, kappa, theta, sigma, t, v1);
        const Real p2
        	= squareRootProcessGreensFct(v_0, kappa, theta, sigma, t, v2);

        // test derivatives
        const Real flowSym2Order = sigma*sigma*v0/(4*h)*(p1-pm1)
                                + (kappa*(v0-theta)+sigma*sigma/2)*p0;

        const Real flowSym4Order
            = sigma*sigma*v0/(24*h)*(-p2 + 8*p1 - 8*pm1 + pm2)
              + (kappa*(v0-theta)+sigma*sigma/2)*p0;

        const Real fwd1Order = sigma*sigma*v0/(2*h)*(p1-p0)
                                + (kappa*(v0-theta)+sigma*sigma/2)*p0;

        const Real fwd2Order = sigma*sigma*v0/(4*h)*(4*p1-3*p0-p2)
                                + (kappa*(v0-theta)+sigma*sigma/2)*p0;

        const Real fwd3Order
            = sigma*sigma*v0/(12*h)*(-p2 + 6*p1 - 3*p0 - 2*pm1)
                                + (kappa*(v0-theta)+sigma*sigma/2)*p0;

        const Real tol = 0.000002;
        if (   std::fabs(expected[i][0] - flowSym2Order) > tol
            || std::fabs(expected[i][1] - flowSym4Order) > tol
            || std::fabs(expected[i][2] - fwd1Order) > tol
            || std::fabs(expected[i][3] - fwd2Order) > tol
            || std::fabs(expected[i][4] - fwd3Order) > tol ) {
            BOOST_ERROR("failed to reproduce Zero Flow BC at"
                       << "\n   v:          " << v
                       << "\n   tolerance:  " << tol);
        }
    }
}


namespace {
    boost::shared_ptr<FdmMesher> createStationaryDistributionMesher(
        Real kappa, Real theta, Real sigma, Size vGrid) {

        const Real qMin = 0.01;
        const Real qMax = 0.99;
        const Real dq = (qMax-qMin)/(vGrid-1);

        std::vector<Real> v(vGrid);
        for (Size i=0; i < vGrid; ++i) {
            v[i] = invStationaryDistributionFct(kappa, theta,
                                                sigma, qMin + i*dq);
        }

        return boost::shared_ptr<FdmMesher>(
            new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                new Predefined1dMesher(v))));
    }
}


void FdHestonTest::testTransformedZeroFlowBC() {
    BOOST_TEST_MESSAGE("Testing zero-flow BC for transformed "
                       "Fokker-Planck forward equation...");

    SavedSettings backup;

    const Real kappa = 1.0;
    const Real theta = 0.4;
    const Real sigma = 2.0;
    const Size vGrid = 100;

    const boost::shared_ptr<FdmMesher> mesher
        = createStationaryDistributionMesher(kappa, theta, sigma, vGrid);
    const Array v = mesher->locations(0);

    Array p(vGrid);
    for (Size i=0; i < v.size(); ++i)
        p[i] =  stationaryProbabilityFct(kappa, theta, sigma, v[i]);


    const Real alpha = 1.0 - 2*kappa*theta/(sigma*sigma);
    const Array q = Pow(v, alpha)*p;

    for (Size i=0; i < vGrid/2; ++i) {
        const Real hm = v[i+1] - v[i];
        const Real hp = v[i+2] - v[i+1];

        const Real eta=1.0/(hm*(hm+hp)*hp);
        const Real a = -eta*(square<Real>()(hm+hp) - hm*hm);
        const Real b  = eta*square<Real>()(hm+hp);
        const Real c = -eta*hm*hm;

        const Real df = a*q[i] + b*q[i+1] + c*q[i+2];
        const Real flow = 0.5*sigma*sigma*v[i]*df + kappa*v[i]*q[i];

        const Real tol = 1e-6;
        if (std::fabs(flow) > tol) {
            BOOST_ERROR("failed to reproduce Zero Flow BC at"
                       << "\n v:          " << v
                       << "\n flow:       " << flow
                       << "\n tolerance:  " << tol);
        }
    }
}

namespace {
    class q_fct : public std::unary_function<Real, Real> {
      public:
        q_fct(const Array& v, const Array& p, const Real alpha)
        : v_(v), q_(Pow(v, alpha)*p), alpha_(alpha) {
            spline_ = boost::shared_ptr<CubicInterpolation>(
                new CubicNaturalSpline(v_.begin(), v_.end(), q_.begin()));
        }

        Real operator()(Real v) {
            return (*spline_)(v, true)*std::pow(v, -alpha_);
        }
      private:

        const Array v_, q_;
        const Real alpha_;
        boost::shared_ptr<CubicInterpolation> spline_;
    };
}

void FdHestonTest::testSquareRootEvolveWithStationaryDensity() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation "
                       "for the square root process with stationary density...");

    // Documentation for this test case:
    // http://www.spanderen.de/2013/05/04/fokker-planck-equation-feller-constraint-and-boundary-conditions/
    SavedSettings backup;

    const Real kappa = 2.5;
    const Real theta = 0.2;
    const Size vGrid = 100;
    const Real eps = 1e-2;

    for (Real sigma = 0.2; sigma < 2.01; sigma+=0.1) {
        const Real alpha = (1.0 - 2*kappa*theta/(sigma*sigma));
        const Real vMin
            = invStationaryDistributionFct(kappa, theta, sigma, eps);
        const Real vMax
            = invStationaryDistributionFct(kappa, theta, sigma, 1-eps);

        const boost::shared_ptr<FdmMesher> mesher(
            new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                    new Uniform1dMesher(vMin, vMax, vGrid))));

        const Array v = mesher->locations(0);
        const FdmSquareRootFwdOp::TransformationType transform = 
            (sigma < 0.75) ? 
               FdmSquareRootFwdOp::Plain : 
               FdmSquareRootFwdOp::Power;

        Array vq (v.size());
        Array vmq(v.size());
        for (Size i=0; i < v.size(); ++i) {
            vmq[i] = 1.0/(vq[i] = std::pow(v[i], alpha));
        }

        Array p(vGrid);
        for (Size i=0; i < v.size(); ++i) {
            p[i] =  stationaryProbabilityFct(kappa, theta, sigma, v[i]);
            if (transform == FdmSquareRootFwdOp::Power)
                p[i] *= vq[i];
        }

        const boost::shared_ptr<FdmSquareRootFwdOp> op(
            new FdmSquareRootFwdOp(mesher, kappa, theta,
                                   sigma, 0, transform));

        const Array eP = p;

        const Size n = 100;
        const Time dt = 0.01;
        DouglasScheme evolver(0.5, op);
        evolver.setStep(dt);

        for (Size i=1; i <= n; ++i) {
            evolver.step(p, i*dt);
        }

        const Real expected = 1-2*eps;

        if (transform == FdmSquareRootFwdOp::Power)        
            for (Size i=0; i < v.size(); ++i) {
                p[i] *= vmq[i];
            }

        const Real calculated = GaussLobattoIntegral(1000000, 1e-6)(
                                        q_fct(v,p,alpha), v.front(), v.back());

        const Real tol = 0.005;
        if (std::fabs(calculated-expected) > tol) {
            BOOST_ERROR("failed to reproduce stationary probability function"
                    << "\n    calculated: " << calculated
                    << "\n    expected:   " << expected
                    << "\n    tolerance:  " << tol);
        }
    }
}

void FdHestonTest::testSquareRootLogEvolveWithStationaryDensity() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation "
                       "for the square root log process with stationary density...");

    // Documentation for this test case:
    // nowhere yet :)
    SavedSettings backup;

    const Real kappa = 2.5;
    const Real theta = 0.2;
    const Size vGrid = 1000;
    const Real eps = 5e-2;

    for (Real sigma = 0.2; sigma < 2.01; sigma+=0.1) {
        //BOOST_TEST_MESSAGE("testing log process sigma =  " << sigma << "\n");
        const Real vMin
            = invStationaryDistributionFct(kappa, theta, sigma, eps);
        const Real vMax
            = invStationaryDistributionFct(kappa, theta, sigma, 1-eps);

        //const boost::shared_ptr<FdmMesher> mesher(
        //    new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
        //            new Uniform1dMesher(log(vgMin), log(vgMax), vGrid))));

        //Real eta = sigma*sigma/kappa/theta/2.0;
        Real beta = 0.075;
        Real betaTheta = 0.02;
        if (sigma > 1.6) {
            beta = 0.01;
            betaTheta = 0.5;
        } else if (sigma > 1.5) {
            beta = 0.01;
            betaTheta = 0.5;
        } else if (sigma > 1.4) {
            //beta = 20.0/(log(vgMax)-log(vgMin))/(pow(eta,5.0));
            beta = 0.03;
            betaTheta = 0.1;
        }
        //BOOST_TEST_MESSAGE("beta " << beta 
        //);
        const std::pair<Real, Real>& cPoints
                     = (std::pair<double, double>(log(vMin), beta));  // 0.075
        std::vector<Real> critialPoints;
        critialPoints.push_back(log(vMin));
        std::vector<boost::tuple<Real, Real, bool> > critPoints;
        critPoints.push_back(boost::tuple<Real, Real, bool>(log(vMin), beta, true));
        critPoints.push_back(boost::tuple<Real, Real, bool>(log(theta), 0.075, true));
        const boost::shared_ptr<FdmMesher> mesher2(
            new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                    new Concentrating1dMesher(log(vMin), log(vMax), vGrid, cPoints, false))));
        const boost::shared_ptr<FdmMesher> mesher(
            new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                    new Concentrating1dMesher(log(vMin), log(vMax), vGrid, critPoints))));
        //const boost::shared_ptr<FdmMesher> mesher(
        //    new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
        //            new MultiConcentrating1dMesher(log(vMin), 
        //                                           log(vMax), 
        //                                           vGrid, 
        //                                           critialPoints, 
        //                                           beta)
        //                                           )));
        
        const Array v = mesher->locations(0);
        const Array mv = mesher2->locations(0);
        //BOOST_TEST_MESSAGE("v[] \n" 
        //    << v << "\n" << mv << "\n " << ((log(vMax)-log(vMin))/0.075)
        //);

        Array p(vGrid);
        for (Size i=0; i < v.size(); ++i)
            p[i] =  stationaryLogProbabilityFct(kappa, theta, sigma, v[i]);

        //BOOST_TEST_MESSAGE("p[] " 
        //    << p << "\n"
        //);

        const boost::shared_ptr<FdmSquareRootFwdOp> op(
            new FdmSquareRootFwdOp(mesher, kappa, theta,
                                   sigma, 0, 
                                   FdmSquareRootFwdOp::Log));

        //BOOST_TEST_MESSAGE("got operator\n");
        const Array eP = p;

        const Size n = 100;
        const Time dt = 0.01;
        FdmBoundaryConditionSet bcSet;
        //bcSet.push_back(boost::shared_ptr<FdmDirichletBoundary>(
        //    new FdmDirichletBoundary(mesher, 0.0, 0,
        //                        FdmDirichletBoundary::Upper)));
        //bcSet.push_back(boost::shared_ptr<FdmDirichletBoundary>(
        //    new FdmDirichletBoundary(mesher, 0.0, 0,
        //                        FdmDirichletBoundary::Lower)));

//        DouglasScheme evolver(0.5, op, bcSet);
        DouglasScheme evolver(0.5, op);
        evolver.setStep(dt);

        for (Size i=1; i <= n; ++i) {
            //const Real alpha = 1-2*kappa*theta/(sigma*sigma);
            //const Array vv(Exp(v));
            //Real calculated = GaussLobattoIntegral(1000000, 1e-6)(
            //                            q_fct(v,p,1), log(vMin), log(vMax));
            //BOOST_TEST_MESSAGE("t " << (i-1)*dt << " integral " << calculated << "\n" 
            //    << p << "\n"
            //);
            evolver.step(p, i*dt);
        }

        const Real expected = 1-2*eps;
        const Real calculated = GaussLobattoIntegral(1000000, 1e-6)(
                                        q_fct(v,p,1), log(vMin), log(vMax));

        const Real calculated1 = GaussLobattoIntegral(1000000, 1e-6)(
                                        q_fct(v,eP,1), log(vMin), log(vMax));

        const Real tol = 0.005;
        BOOST_TEST_MESSAGE("sigma " << sigma << ", " << calculated-expected 
        );
        if (std::fabs(calculated-expected) > tol) {
            BOOST_ERROR("failed to reproduce stationary probability function for "
                    << "\n    sigma:      " << sigma
                    << "\n    calculated1:" << calculated1
                    << "\n    calculated: " << calculated
                    << "\n    expected:   " << expected
                    << "\n    tolerance:  " << tol);
        }
    }
}

void FdHestonTest::testSquareRootFokkerPlanckFwdEquation() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation "
                       "for the square root process with Dirac start...");

    SavedSettings backup;

    const Real kappa = 1.2;
    const Real theta = 0.4;
    const Real sigma = 0.7;
    const Real v0 = theta;
    const Real alpha = 1.0 - 2*kappa*theta/(sigma*sigma);

    const Time maturity = 1.0;

    const Size xGrid = 1001;
    const Size tGrid = 500;

    const Real vol = sigma*std::sqrt(theta/(2*kappa));
    const Real upperBound = theta+6*vol;
    const Real lowerBound = std::max(0.0002, theta-6*vol);

    const boost::shared_ptr<FdmMesher> mesher(
        new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                new Uniform1dMesher(lowerBound, upperBound, xGrid))));

    const Array x(mesher->locations(0));

    const boost::shared_ptr<FdmSquareRootFwdOp> op(
        new FdmSquareRootFwdOp(mesher, kappa, theta, sigma, 0)); //!

    const Time dt = maturity/tGrid;
    const Size n = 5;

    Array p(xGrid);
    for (Size i=0; i < p.size(); ++i) {
        p[i] = squareRootProcessGreensFct(v0, kappa, theta,
                                   sigma, n*dt, x[i]);
    }
    Array q = Pow(x, alpha)*p;

    DouglasScheme evolver(0.5, op);
    evolver.setStep(dt);

    for (Time t=(n+1)*dt; t <= maturity+20*QL_EPSILON; t+=dt) {
        evolver.step(p, t);
        evolver.step(q, t);
    }

    const Real tol = 0.002;

    Array y(x.size());
    for (Size i=0; i < x.size(); ++i) {
        const Real expected = squareRootProcessGreensFct(v0, kappa, theta,
                                                  sigma, maturity, x[i]);

        const Real calculated = p[i];
        if (std::fabs(expected - calculated) > tol) {
            BOOST_FAIL("failed to reproduce pdf at"
                       << QL_FIXED << std::setprecision(5)
                       << "\n   x:          " << x[i]
                       << "\n   calculated: " << calculated
                       << "\n   expected:   " << expected
                       << "\n   tolerance:  " << tol);
        }
    }
}



namespace {
    Real fokkerPlanckPrice2D(const Array& p,
                       const boost::shared_ptr<FdmMesherComposite>& mesher) {

        std::vector<Real> x, y;
        const boost::shared_ptr<FdmLinearOpLayout> layout = mesher->layout();

        x.reserve(layout->dim()[0]);
        y.reserve(layout->dim()[1]);

        const FdmLinearOpIterator endIter = layout->end();
        for (FdmLinearOpIterator iter = layout->begin(); iter != endIter;
              ++iter) {
            if (!iter.coordinates()[1]) {
                x.push_back(mesher->location(iter, 0));
            }
            if (!iter.coordinates()[0]) {
                y.push_back(mesher->location(iter, 1));
            }
        }

        return FdmMesherIntegral(mesher,
                                 DiscreteSimpsonIntegral()).integrate(p);
    }

    Real hestonPxBoundary(
    	Time maturity, Real eps,
    	const boost::shared_ptr<HestonModel>& model) {

        const AnalyticPDFHestonEngine pdfEngine(model);
        const Real sInit = model->process()->s0()->value();
        const Real xMin = Brent().solve(
        	boost::bind(std::minus<Real>(),
        		boost::bind(&AnalyticPDFHestonEngine::cdf,
        					&pdfEngine, _1, maturity), eps),
        				sInit*1e-3, sInit, sInit*0.001, 1000*sInit);

        return xMin;
    }

    struct FokkerPlanckFwdTestCase {
    	const Real s0, r, q, v0, kappa, theta, rho, sigma;
    	const Size xGrid, vGrid, tGridPerYear;
    	const FdmSquareRootFwdOp::TransformationType trafoType;
    	const FdmHestonGreensFct::Algorithm greensAlgorithm;
    };

    void hestonFokkerPlanckFwdEquationTest(
    	const FokkerPlanckFwdTestCase& testCase) {

        SavedSettings backup;

        const DayCounter dc = ActualActual();
        const Date todaysDate = Date(28, Dec, 2014);
        Settings::instance().evaluationDate() = todaysDate;

        std::vector<Period> maturities;
        maturities+=Period(1, Months),
        			Period(3, Months), Period(6, Months), Period(9, Months),
        			Period(1, Years), Period(2, Years), Period(3, Years),
        			Period(4, Years), Period(5, Years);

        const Date maturityDate = todaysDate + maturities.back();
        const Time maturity = dc.yearFraction(todaysDate, maturityDate);

        const Real s0 = testCase.s0;
        const Real x0 = std::log(s0);
        const Rate r = testCase.r;
        const Rate q = testCase.q;

        const Real kappa = testCase.kappa;
        const Real theta = testCase.theta;
        const Real rho   = testCase.rho;
        const Real sigma = testCase.sigma;
        const Real v0    = testCase.v0;
        const Real alpha = 1.0 - 2*kappa*theta/(sigma*sigma);

        const Handle<Quote> spot(boost::shared_ptr<Quote>(new SimpleQuote(s0)));
        const Handle<YieldTermStructure> rTS(flatRate(r, dc));
        const Handle<YieldTermStructure> qTS(flatRate(q, dc));

        const boost::shared_ptr<HestonProcess> process(
            new HestonProcess(rTS, qTS, spot, v0, kappa, theta, sigma, rho));

        const boost::shared_ptr<HestonModel> model(new HestonModel(process));

        const boost::shared_ptr<PricingEngine> engine(
            new AnalyticHestonEngine(model));

        const Size xGrid = testCase.xGrid;
        const Size vGrid = testCase.vGrid;
        const Size tGridPerYear = testCase.tGridPerYear;

        const FdmSquareRootFwdOp::TransformationType transformationType
            = testCase.trafoType;
        Real lowerBound, upperBound;
        std::vector<boost::tuple<Real, Real, bool> > cPoints;

        switch (transformationType) {
        case FdmSquareRootFwdOp::Log:
          {
        	upperBound = std::log(
        		invStationaryDistributionFct(kappa, theta, sigma, 0.9995));
            lowerBound = std::log(0.00001);

            const Real v0Center = std::log(v0);
            const Real v0Density = 10.0;
            const Real upperBoundDensity = 100;
            const Real lowerBoundDensity = 1.0;
            cPoints += boost::make_tuple(lowerBound, lowerBoundDensity, false);
            		   boost::make_tuple(v0Center, v0Density, true),
            		   boost::make_tuple(upperBound, upperBoundDensity, false);
          }
        break;
        case FdmSquareRootFwdOp::Plain:
          {
        	upperBound =
        		invStationaryDistributionFct(kappa, theta, sigma, 0.9995);
        	lowerBound = invStationaryDistributionFct(kappa, theta, sigma, 1e-5);

            const Real v0Center = v0;
            const Real v0Density = 1000.0;
            const Real lowerBoundDensity = 1.0;
            cPoints += boost::make_tuple(lowerBound, lowerBoundDensity, false),
            		   boost::make_tuple(v0Center, v0Density, true);
          }
        break;
        case FdmSquareRootFwdOp::Power:
          {
          	upperBound =
          		invStationaryDistributionFct(kappa, theta, sigma, 0.999);
            lowerBound = 0.0001;

            const Real v0Center = v0;
            const Real v0Density = 10.0;
            const Real lowerBoundDensity = 1000;
            cPoints += boost::make_tuple(lowerBound, lowerBoundDensity, false),
            		   boost::make_tuple(v0Center, v0Density, true);
          }
        break;
        default:
        	QL_FAIL("unknown transformation type");
        }

        const boost::shared_ptr<Fdm1dMesher> varianceMesher(
            new Concentrating1dMesher(lowerBound, upperBound,
            						  vGrid, cPoints, 1e-12));

        const Real sEps = 1e-5;
        const Real sLowerBound
        	= std::log(hestonPxBoundary(maturity, sEps, model));
        const Real sUpperBound
        	= std::log(hestonPxBoundary(maturity, 1-sEps,model));

        const boost::shared_ptr<Fdm1dMesher> spotMesher(
        	new Concentrating1dMesher(sLowerBound, sUpperBound, xGrid,
        		std::make_pair(x0, 0.1), true));

        const boost::shared_ptr<FdmMesherComposite>
            mesher(new FdmMesherComposite(spotMesher, varianceMesher));

    	const boost::shared_ptr<FdmLinearOpComposite> hestonFwdOp(
    		new FdmHestonFwdOp(mesher, process, transformationType));

        HundsdorferScheme evolver(FdmSchemeDesc::Hundsdorfer().theta,
                                  FdmSchemeDesc::Hundsdorfer().mu, hestonFwdOp);

        // step one days using non-correlated process
        const Time eT = 2.0/365;
        Array p = FdmHestonGreensFct(mesher, process, testCase.trafoType)
        		.get(eT, testCase.greensAlgorithm);

        const boost::shared_ptr<FdmLinearOpLayout> layout = mesher->layout();
        const Real strikes[] = { 50, 80, 90, 100, 110, 120, 150, 200 };

        std::cout << "expiry date\t avg diff\t"
        		  << " min diff\t max diff" << std::endl;

        Time t=eT;
        for (std::vector<Period>::const_iterator iter = maturities.begin();
        		iter != maturities.end(); ++iter) {

        	// calculate step size
        	const Date nextMaturityDate = todaysDate + *iter;
        	const Time nextMaturityTime
        		= dc.yearFraction(todaysDate, nextMaturityDate);

        	Time dt = (nextMaturityTime - t)/tGridPerYear;
        	evolver.setStep(dt);

    		for (Size i=0; i < tGridPerYear; ++i, t+=dt) {
    			evolver.step(p, t+dt);
    		}

    		Real avg=0, min=QL_MAX_REAL, max=0;
    		for (Size i=0; i < LENGTH(strikes); ++i) {
    			const Real strike = strikes[i];
    			const boost::shared_ptr<StrikedTypePayoff> payoff(
    				new PlainVanillaPayoff((strike > s0) ? Option::Call :
    													   Option::Put, strike));

    			Array pd(p.size());
    			for (FdmLinearOpIterator iter = layout->begin();
    				iter != layout->end(); ++iter) {
    				const Size idx = iter.index();
    				const Real s = std::exp(mesher->location(iter, 0));

    				pd[idx] = payoff->operator()(s)*p[idx];
    		        if (transformationType == FdmSquareRootFwdOp::Power) {
    		            const Real v = mesher->location(iter, 1);
    		            pd[idx] *= std::pow(v, -alpha);
    		        }
    			}

    			const Real calculated = fokkerPlanckPrice2D(pd, mesher)
    				* rTS->discount(nextMaturityDate);

    		    const boost::shared_ptr<Exercise> exercise(
    		        new EuropeanExercise(nextMaturityDate));

    			VanillaOption option(payoff, exercise);
    			option.setPricingEngine(engine);

    			const Real expected = option.NPV();
    			const Real diff = std::fabs(expected - calculated);

    			avg+=diff;
    			min=std::min(diff, min);
    			max=std::max(diff, max);
    		}

    		std::cout << io::iso_date(nextMaturityDate) << "\t "
    				  << QL_FIXED << std::setprecision(5)
    				  << avg/LENGTH(strikes) << "\t "
    				  << min << "\t " << max << std::endl;
        }

    }
}

void FdHestonTest::testHestonFokkerPlanckFwdEquation() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation "
                       "for the Heston process...");

    FokkerPlanckFwdTestCase testCases[] = {
		{
			100.0, 0.01, 0.02,
			0.05, 1.0, 0.05, -0.75, std::sqrt(0.2),
			201, 4001, 25,
			FdmSquareRootFwdOp::Power,
			FdmHestonGreensFct::Gaussian
		},
    	{
			100.0, 0.01, 0.02,
			0.05, 1.0, 0.05, -0.75, std::sqrt(0.2),
			201, 501, 25,
			FdmSquareRootFwdOp::Log,
			FdmHestonGreensFct::Gaussian
    	},
       	{
			100.0, 0.01, 0.02,
			0.05, 1.0, 0.05, -0.75, std::sqrt(0.2),
			201, 501, 25,
			FdmSquareRootFwdOp::Log,
			FdmHestonGreensFct::ZeroCorrelation
       	},
		{
			100.0, 0.01, 0.02,
			0.05, 1.0, 0.05, -0.75, std::sqrt(0.005),
			401, 501, 25,
			FdmSquareRootFwdOp::Plain,
			FdmHestonGreensFct::Gaussian
		}
    };

    for (Size i=0; i < LENGTH(testCases); ++i) {
    	hestonFokkerPlanckFwdEquationTest(testCases[i]);
    }
}


namespace {
    boost::shared_ptr<BicubicSpline> createFlatLeverageFct(Matrix & surface, const std::vector<Real> & strikes, const std::vector<Real> & times, Real flatVol) {
        for (Size i=0; i < strikes.size(); ++i)
            for (Size j=0; j < times.size(); ++j) {
                surface[i][j] = flatVol;
            }

        boost::shared_ptr<BicubicSpline> leverage = boost::shared_ptr<BicubicSpline> (
                new BicubicSpline(times.begin(), times.end(),
                                  strikes.begin(), strikes.end(),
                                  surface));
        return leverage;
    }

    boost::shared_ptr<BicubicSpline> createLeverageFctFromVolSurface(
            boost::shared_ptr<BlackScholesMertonProcess> lvProcess,
            const std::vector<Real>& strikes,
            const std::vector<Date>& dates,
            std::vector<Time>& times,
            Matrix & surface) {
        std::cout << "calculating lv surface" << std::endl;
        const boost::shared_ptr<LocalVolTermStructure> localVol
        	= lvProcess->localVolatility().currentLink();

        const DayCounter dc = localVol->dayCounter();
        const Date todaysDate = Settings::instance().evaluationDate();

        QL_REQUIRE(times.size() == dates.size(), "mismatch");

        for (Size i=0; i < times.size(); ++i) {
        	times[i] = dc.yearFraction(todaysDate, dates[i]);
        }

        std::cout << "got link" << std::endl;
        for (Size i=0; i < strikes.size(); ++i) {
            std::cout << "spot: " << strikes[i] << " : ";
            for (Size j=0; j < dates.size(); ++j) {
                try {
                    surface[i][j] = localVol->localVol(dates[j], strikes[i], true);
                } catch (Error&) {
                    surface[i][j] = 0.2;
                }
                std::cout //<< "(" << i << ", " << j << ") (" << strikes[i] << ", " << times[j] << ") : "
                    << surface[i][j] << ", " ; // << std::endl;
            }
            std::cout << std::endl;
        }

        boost::shared_ptr<BicubicSpline> leverage
        	= boost::shared_ptr<BicubicSpline> (
                new BicubicSpline(times.begin(), times.end(),
                                  strikes.begin(), strikes.end(),
                                  surface));
        leverage->disableExtrapolation();
        return leverage;
    }

    boost::tuple<std::vector<Real>, std::vector<Date>,
    			 boost::shared_ptr<BlackVarianceSurface> >
    	createSmoothImpliedVol(const DayCounter& dc, const Calendar& cal) {

    	const Date todaysDate = Settings::instance().evaluationDate();

        Integer times[] = { 13, 41, 75, 165, 256, 345, 524, 703 };
        std::vector<Date> dates;
        for (Size i = 0; i < 8; ++i) {
            Date date = todaysDate + times[i];
            dates.push_back(date);
        }

        Real tmp[] = { 2.222222222, 11.11111111, 44.44444444, 75.55555556, 80, 84.44444444, 88.88888889, 93.33333333, 97.77777778, 100,
                       102.2222222, 106.6666667, 111.1111111, 115.5555556, 120, 124.4444444, 166.6666667, 222.2222222, 444.4444444, 666.6666667
             };
        const std::vector<Real> surfaceStrikes(tmp, tmp+LENGTH(tmp));

		Volatility v[] =
		  { 1.015873, 1.015873, 1.015873, 0.89729, 0.796493, 0.730914, 0.631335, 0.568895,
			0.711309, 0.711309, 0.711309, 0.641309, 0.635593, 0.583653, 0.508045, 0.463182,
			0.516034, 0.500534, 0.500534, 0.500534, 0.448706, 0.416661, 0.375470, 0.353442,
			0.516034, 0.482263, 0.447713, 0.387703, 0.355064, 0.337438, 0.316966, 0.306859,
			0.497587, 0.464373, 0.430764, 0.374052, 0.344336, 0.328607, 0.310619, 0.301865,
			0.479511, 0.446815, 0.414194, 0.361010, 0.334204, 0.320301, 0.304664, 0.297180,
			0.461866, 0.429645, 0.398092, 0.348638, 0.324680, 0.312512, 0.299082, 0.292785,
			0.444801, 0.413014, 0.382634, 0.337026, 0.315788, 0.305239, 0.293855, 0.288660,
			0.428604, 0.397219, 0.368109, 0.326282, 0.307555, 0.298483, 0.288972, 0.284791,
			0.420971, 0.389782, 0.361317, 0.321274, 0.303697, 0.295302, 0.286655, 0.282948,
			0.413749, 0.382754, 0.354917, 0.316532, 0.300016, 0.292251, 0.284420, 0.281164,
			0.400889, 0.370272, 0.343525, 0.307904, 0.293204, 0.286549, 0.280189, 0.277767,
			0.390685, 0.360399, 0.334344, 0.300507, 0.287149, 0.281380, 0.276271, 0.274588,
			0.383477, 0.353434, 0.327580, 0.294408, 0.281867, 0.276746, 0.272655, 0.271617,
			0.379106, 0.349214, 0.323160, 0.289618, 0.277362, 0.272641, 0.269332, 0.268846,
			0.377073, 0.347258, 0.320776, 0.286077, 0.273617, 0.269057, 0.266293, 0.266265,
			0.399925, 0.369232, 0.338895, 0.289042, 0.265509, 0.255589, 0.249308, 0.249665,
			0.423432, 0.406891, 0.373720, 0.314667, 0.281009, 0.263281, 0.246451, 0.242166,
			0.453704, 0.453704, 0.453704, 0.381255, 0.334578, 0.305527, 0.268909, 0.251367,
			0.517748, 0.517748, 0.517748, 0.416577, 0.364770, 0.331595, 0.287423, 0.264285 };

		Matrix blackVolMatrix(surfaceStrikes.size(), dates.size());
		for (Size i=0; i < surfaceStrikes.size(); ++i)
			for (Size j=0; j < dates.size(); ++j) {
				blackVolMatrix[i][j] = v[i*(dates.size())+j];
			}

		const boost::shared_ptr<BlackVarianceSurface> volTS(
			new BlackVarianceSurface(todaysDate, cal,
									 dates,
									 surfaceStrikes, blackVolMatrix,
									 dc));
		volTS->setInterpolation<Bicubic>();

		return boost::make_tuple(surfaceStrikes, dates, volTS);
    }
}

void FdHestonTest::testHestonFokkerPlanckFwdEquationLogLVLeverage() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation "
                       "for the Heston process Log Transformation with leverage LV limiting case...");

    SavedSettings backup;

    const DayCounter dc = ActualActual();
    //const Date todaysDate = Date(28, Dec, 2012);
    const Date todaysDate(5, July, 2002);
    Settings::instance().evaluationDate() = todaysDate;

    const Date maturityDate = todaysDate + Period(1, Years);
    const Time maturity = dc.yearFraction(todaysDate, maturityDate);

    const Real s0 = 100;
    const Real x0 = std::log(s0);
    const Rate r = 0.0;
    const Rate q = 0.00;

    const Real kappa =  1.0;
    const Real theta =  1.0;
    const Real rho   = -0.0;
    const Real sigma =  0.01;
    const Real v0    =  theta;

    const FdmSquareRootFwdOp::TransformationType transform
    	= FdmSquareRootFwdOp::Plain;

    const DayCounter dayCounter = Actual365Fixed();
    const Calendar calendar = TARGET();

    const Handle<Quote> spot(boost::shared_ptr<Quote>(new SimpleQuote(s0)));
    const Handle<YieldTermStructure> rTS(flatRate(todaysDate, r, dayCounter));
    const Handle<YieldTermStructure> qTS(flatRate(todaysDate, q, dayCounter));

    boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, spot, v0, kappa, theta, sigma, rho));

    const Size xGrid = 201;
    const Size vGrid = 501;
    const Size tGrid = 50;

    const Real upperBound
        = invStationaryDistributionFct(kappa, theta, sigma, 0.99);
    const Real lowerBound
    	= invStationaryDistributionFct(kappa, theta, sigma, 0.01);

    const Real beta = 10.0;
    std::vector<boost::tuple<Real, Real, bool> > critPoints;
    critPoints.push_back(boost::tuple<Real, Real, bool>(lowerBound, beta, true));
    critPoints.push_back(boost::tuple<Real, Real, bool>(v0, beta/100, true));
    critPoints.push_back(boost::tuple<Real, Real, bool>(upperBound, beta, true));
    const boost::shared_ptr<Fdm1dMesher> varianceMesher(
            new Concentrating1dMesher(lowerBound, upperBound, vGrid, critPoints));

//    const boost::shared_ptr<Fdm1dMesher> equityMesher(
//        new FdmBlackScholesMesher(
//            xGrid,
//            FdmBlackScholesMesher::processHelper(
//              hestonProcess->s0(), hestonProcess->dividendYield(),
//              hestonProcess->riskFreeRate(), 2.0),
//              maturity, s0));

    const boost::shared_ptr<Fdm1dMesher> equityMesher(
    	new Concentrating1dMesher(std::log(1), std::log(600.0), xGrid,
    		std::make_pair(x0, 0.001), true));

    const boost::shared_ptr<FdmMesherComposite>
        mesher(new FdmMesherComposite(equityMesher, varianceMesher));

    const boost::tuple<std::vector<Real>, std::vector<Date>,
        	     boost::shared_ptr<BlackVarianceSurface> > smoothSurface =
        createSmoothImpliedVol(dayCounter, calendar);
    const boost::shared_ptr<BlackScholesMertonProcess> lvProcess(
        new BlackScholesMertonProcess(spot, qTS, rTS,
        	Handle<BlackVolTermStructure>(smoothSurface.get<2>())));

    // step two days using non-correlated process
    const Time eT = 2.0/365;
    Array p(mesher->layout()->size(), 0.0);
    //BOOST_TEST_MESSAGE("smooting delta function.."
    //);

    Real v=-Null<Real>(), p_v;
    const Real bsV0 = square<Real>()(
    	lvProcess->blackVolatility()->blackVol(0.0, s0, true));

    const boost::shared_ptr<FdmLinearOpLayout> layout = mesher->layout();
    for (FdmLinearOpIterator iter = layout->begin(); iter != layout->end();
        ++iter) {
        const Real x = mesher->location(iter, 0);
        if (v != mesher->location(iter, 1)) {
        	v = mesher->location(iter, 1);
        	p_v = squareRootProcessGreensFct(v0, kappa, theta, sigma, eT, v);
        }
        const Real p_x = 1.0/(std::sqrt(M_TWOPI*bsV0*eT))
                * std::exp(-0.5*square<Real>()(x - x0)/(bsV0*eT));
        p[iter.index()] = p_v*p_x;
    }
    const Time dt = (maturity-eT)/tGrid;


    //--- test LV/leverage

    Real denseStrikes[] =
    { 2.222222222, 11.11111111, 20, 25, 30, 35, 40, 44.44444444, 50, 55, 60, 65, 70, 75.55555556, 80, 84.44444444, 88.88888889, 93.33333333, 97.77777778, 100,
      102.2222222, 106.6666667, 111.1111111, 115.5555556, 120, 124.4444444, 166.6666667, 222.2222222, 444.4444444, 666.6666667 };

    const std::vector<Real> ds(denseStrikes, denseStrikes+LENGTH(denseStrikes));

    Matrix surface(ds.size(), smoothSurface.get<1>().size());
    std::vector<Time> times(surface.columns());

    boost::shared_ptr<BicubicSpline> leverage
    	= createLeverageFctFromVolSurface(lvProcess, ds,
    		smoothSurface.get<1>(), times, surface);

    const boost::shared_ptr<PricingEngine> lvEngine(
        new AnalyticEuropeanEngine(lvProcess));
    //--- LV structures for comparison calculation

    const boost::shared_ptr<FdmLinearOpComposite> hestonFwdOp(
        new FdmHestonFwdOp(mesher, hestonProcess, transform, leverage));

    HundsdorferScheme evolver(FdmSchemeDesc::Hundsdorfer().theta,
                              FdmSchemeDesc::Hundsdorfer().mu,
                              hestonFwdOp);

    Time t=dt;
    evolver.setStep(dt);

    BOOST_TEST_MESSAGE("start evolve\n");
    for (Size i=0; i < tGrid; ++i, t+=dt) {
        evolver.step(p, t);
    }
    BOOST_TEST_MESSAGE("finished evolve\n");

    const boost::shared_ptr<Exercise> exercise(
        new EuropeanExercise(maturityDate));

    //const Real strikes[] = { 44.4, 80, 93.3, 100, 111.1, 120, 166.6, 222.2 };

    //for (Size s=0; i < LENGTH(strikes); ++i) {
    for (Size strike=3; strike < 200.0; ++strike) {
        //const Real strike = strikes[i];

        const boost::shared_ptr<StrikedTypePayoff> payoff(
            new CashOrNothingPayoff(Option::Put, strike, 1.0));


//        const boost::shared_ptr<StrikedTypePayoff> payoff(
//            new PlainVanillaPayoff((strike > s0) ? Option::Call :
//                                                   Option::Put, strike));

        Array pd(p.size());
        for (FdmLinearOpIterator iter = layout->begin();
            iter != layout->end(); ++iter) {
            const Size idx = iter.index();
            const Real s = std::exp(mesher->location(iter, 0));

            pd[idx] = payoff->operator()(s)*p[idx];
        }

        const Real calculated
            = fokkerPlanckPrice2D(pd, mesher)*rTS->discount(maturityDate);

        const boost::shared_ptr<PricingEngine> fdmEngine(
        	new FdBlackScholesVanillaEngine(lvProcess, 25, 200, 0,
        									FdmSchemeDesc::Douglas(), true,0.2));

        VanillaOption option(payoff, exercise);
//        option.setPricingEngine(fdmEngine);
//        const Real expectedHeston = option.NPV();
        option.setPricingEngine(fdmEngine);
        const Real expectedLV = option.NPV();
        BOOST_TEST_MESSAGE("strike " << strike << " " << calculated << " " << expectedLV);// << ",\t (" << expectedHeston << ")");

        //const Real tol = 1000.1;
        //if (std::fabs(expected - calculated ) > tol) {
        //    BOOST_FAIL("failed to reproduce Heston prices at"
        //               << "\n   strike      " << strike
        //               << QL_FIXED << std::setprecision(5)
        //               << "\n   calculated: " << calculated
        //               << "\n   expected:   " << expected
        //               << "\n   tolerance:  " << tol);
        //}
    }
}


void FdHestonTest::testBlackScholesFokkerPlanckFwdEquationLocalVol() {
    BOOST_TEST_MESSAGE("Testing Fokker-Planck forward equation for BS Local Vol process...");

    SavedSettings backup;

    const DayCounter dc = ActualActual();
    const Date todaysDate(5, July, 2014);
    Settings::instance().evaluationDate() = todaysDate;

    const Real s0 = 100;
    const Real x0 = std::log(s0);
    const Rate r = 0.035;
    const Rate q = 0.01;

    const Calendar calendar = TARGET();
    const DayCounter dayCounter = Actual365Fixed();

    const boost::shared_ptr<YieldTermStructure> rTS1(
        flatRate(todaysDate, r, dayCounter));
    const boost::shared_ptr<YieldTermStructure> qTS1(
        flatRate(todaysDate, q, dayCounter));

    boost::tuple<std::vector<Real>, std::vector<Date>,
        	     boost::shared_ptr<BlackVarianceSurface> > smoothImpliedVol =
   		createSmoothImpliedVol(dayCounter, calendar);

    const std::vector<Real>& strikes = smoothImpliedVol.get<0>();
    const std::vector<Date>& dates = smoothImpliedVol.get<1>();
    const Handle<BlackVolTermStructure> vTS = Handle<BlackVolTermStructure>(
    	createSmoothImpliedVol(dayCounter, calendar).get<2>());

    const Size xGrid = 2*100+1;
    const Size tGrid = 400;

    const Handle<Quote> spot(boost::shared_ptr<Quote>(new SimpleQuote(s0)));
    const Handle<YieldTermStructure> qTS(qTS1);
    const Handle<YieldTermStructure> rTS(rTS1);
    const boost::shared_ptr<BlackScholesMertonProcess> process(
        new BlackScholesMertonProcess(spot, qTS, rTS, vTS));

    const boost::shared_ptr<PricingEngine> engine(
        new AnalyticEuropeanEngine(process));

    for (Size i=1; i < dates.size(); ++i) {
        for (Size j=3; j < strikes.size()-5; j+=5) {

            const Date& exDate = dates[i];
            const Date maturityDate = exDate;
            const Time maturity = dc.yearFraction(todaysDate, maturityDate);
            const boost::shared_ptr<Exercise> exercise(
                                                 new EuropeanExercise(exDate));

            const boost::shared_ptr<FdmMesher> uniformMesher(
                new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                    new FdmBlackScholesMesher(xGrid, process, maturity, s0))));
            //-- operator --
            const boost::shared_ptr<FdmLinearOpComposite> uniformBSFwdOp(
                new FdmBlackScholesFwdOp(uniformMesher, process, s0, true, 0.2));

            const boost::shared_ptr<FdmMesher> concentratedMesher(
                new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                    new FdmBlackScholesMesher(xGrid, process, maturity, s0,
                                              Null<Real>(), Null<Real>(), 0.0001, 1.5,
                                              std::pair<Real, Real>(s0, 0.1)))));

            //-- operator --
            const boost::shared_ptr<FdmLinearOpComposite> concentratedBSFwdOp(
                new FdmBlackScholesFwdOp(concentratedMesher, process, s0, true, 0.2));

            const boost::shared_ptr<FdmMesher> shiftedMesher(
                new FdmMesherComposite(boost::shared_ptr<Fdm1dMesher>(
                    new FdmBlackScholesMesher(xGrid, process, maturity, s0,
                                              Null<Real>(), Null<Real>(), 0.0001, 1.5,
                                              std::pair<Real, Real>(s0*1.1, 0.2)))));

            //-- operator --
            const boost::shared_ptr<FdmLinearOpComposite> shiftedBSFwdOp(
                new FdmBlackScholesFwdOp(shiftedMesher, process, s0, true, 0.2));

            const boost::shared_ptr<StrikedTypePayoff> payoff(
                new PlainVanillaPayoff(Option::Call, strikes[j]));

            VanillaOption option(payoff, exercise);
            option.setPricingEngine(engine);

            const Real expected = option.NPV();
            const Real calcUniform
                = fokkerPlanckPrice1D(uniformMesher, uniformBSFwdOp,
                                      payoff, x0, maturity, tGrid)*rTS->discount(maturityDate);
            const Real calcConcentrated
                = fokkerPlanckPrice1D(concentratedMesher, concentratedBSFwdOp,
                                      payoff, x0, maturity, tGrid)*rTS->discount(maturityDate);
            const Real calcShifted
                = fokkerPlanckPrice1D(shiftedMesher, shiftedBSFwdOp,
                                      payoff, x0, maturity, tGrid)*rTS->discount(maturityDate);
            const Real tol = 0.05;

            BOOST_TEST_MESSAGE("date " << dates[i] << " strike " << strikes[j] << ", " << calcUniform  << ", " << calcConcentrated  << ", " << calcShifted << ", " << expected
            );
            if (std::fabs(expected - calcUniform) > tol) {
                BOOST_FAIL("failed to reproduce european option price "
                           << "with an uniform mesher"
                           << "\n   strike:     " << strikes[i]
                           << QL_FIXED << std::setprecision(8)
                           << "\n   calculated: " << calcUniform
                           << "\n   expected:   " << expected
                           << "\n   tolerance:  " << tol);
            }
            if (std::fabs(expected - calcConcentrated) > tol) {
                BOOST_FAIL("failed to reproduce european option price "
                           << "with a concentrated mesher"
                           << "\n   strike:     " << strikes[i]
                           << QL_FIXED << std::setprecision(8)
                           << "\n   calculated: " << calcConcentrated
                           << "\n   expected:   " << expected
                           << "\n   tolerance:  " << tol);
            }
            if (std::fabs(expected - calcShifted) > tol) {
                BOOST_FAIL("failed to reproduce european option price "
                           << "with a shifted mesher"
                           << "\n   strike:     " << strikes[i]
                           << QL_FIXED << std::setprecision(8)
                           << "\n   calculated: " << calcShifted
                           << "\n   expected:   " << expected
                           << "\n   tolerance:  " << tol);
            }
        }
    }
}

namespace {
	Volatility safeLocalVolAccess(
		const boost::shared_ptr<LocalVolSurface>& localVol,
		Time t,	Real spot, Volatility override = 0.2) {

		return 0.3;

		try {
			return localVol->localVol(t, spot, true);
		}
		catch (Error&) {
           return override;
		}
	}
}


void FdHestonTest::testLSVCalibration() {
    BOOST_TEST_MESSAGE("Testing stochastic local volatility calibration...");

    SavedSettings backup;

    const Date todaysDate(5, July, 2014);
    Settings::instance().evaluationDate() = todaysDate;

    const Calendar calendar = TARGET();
    const DayCounter dayCounter = Actual365Fixed();

    const Size nMonths = 6*12;
    std::vector<Date> maturityDates;
    std::vector<Time> maturities;
    maturities.reserve(nMonths);
    maturityDates.reserve(nMonths);
    for (Size i=0; i < nMonths; ++i) {
    	maturityDates.push_back(todaysDate + Period(i, Months));
    	maturities.push_back(
    		dayCounter.yearFraction(todaysDate, maturityDates.back()));
    }
    const Time maturity = maturities.back();

    const Real s0 = 100;
    const Real x0 = std::log(s0);
    const Handle<Quote> spot(boost::shared_ptr<Quote>(new SimpleQuote(s0)));

    const Rate r = 0.035;
    const Rate q = 0.01;

    const Real v0    = 0.195662;
    const Real kappa = 1.0;
    const Real theta = 0.3; //0.0745911;
    const Real sigma = 0.25;//0.4882;
    const Real rho   =-0.511493;

    const Handle<YieldTermStructure> rTS(
        flatRate(todaysDate, r, dayCounter));
    const Handle<YieldTermStructure> qTS(
        flatRate(todaysDate, q, dayCounter));

    const boost::shared_ptr<HestonProcess> hestonProcess(
        new HestonProcess(rTS, qTS, spot, v0, kappa, theta, sigma, rho));

    const boost::shared_ptr<HestonModel> hestonModel(
    	new HestonModel(hestonProcess));

    const boost::tuple<std::vector<Real>, std::vector<Date>,
        	     boost::shared_ptr<BlackVarianceSurface> > smoothImpliedVol =
   		createSmoothImpliedVol(dayCounter, calendar);

    const boost::shared_ptr<LocalVolSurface> localVol(
    	new LocalVolSurface(
    		Handle<BlackVolTermStructure>(smoothImpliedVol.get<2>()),
        	rTS, qTS, spot));

    const Size xGrid = 201;
    const Size vGrid = 501;
    const Size tGridPerYear = 50;

    const FdmSquareRootFwdOp::TransformationType trafoType
        = FdmSquareRootFwdOp::Plain;

    std::vector<boost::tuple<Real, Real, bool> > cPoints;

    const Real upperBound =
    		invStationaryDistributionFct(kappa, theta, sigma, 0.995);
    const Real lowerBound =
    		invStationaryDistributionFct(kappa, theta, sigma, 1e-5);

	const Real v0Center = v0;
	const Real v0Density = 10.0;
	cPoints += boost::make_tuple(v0Center, v0Density, true);

    const boost::shared_ptr<Fdm1dMesher> varianceMesher(
        new Concentrating1dMesher(lowerBound, upperBound,
        						  vGrid, cPoints, 1e-8));

    const Real sEps = 1e-4;
    const Real sLowerBound
    	= std::log(hestonPxBoundary(maturity, sEps, hestonModel));
    const Real sUpperBound
    	= std::log(hestonPxBoundary(maturity, 1-sEps, hestonModel));

    const boost::shared_ptr<Fdm1dMesher> spotMesher(
    	new Concentrating1dMesher(sLowerBound, sUpperBound, xGrid,
    		std::make_pair(x0, 0.1), true));

    const boost::shared_ptr<FdmMesherComposite>
        mesher(new FdmMesherComposite(spotMesher, varianceMesher));

    const Time eT = 2.0/365;
    // greens function uses ATM local vol for the equity part
    const Volatility atmLv = localVol->localVol(0.0, s0, true);
    Array p = FdmHestonGreensFct(mesher,
    	boost::shared_ptr<HestonProcess>(new HestonProcess(
    	    rTS, qTS, spot, atmLv*atmLv, kappa, theta, sigma, rho)),
    	    trafoType).get(eT, FdmHestonGreensFct::Gaussian);

    std::vector<Time> mandatoryTimeSteps(1, eT);
    std::copy(maturities.begin(), maturities.end(),
    		  std::back_inserter(mandatoryTimeSteps));
    TimeGrid timeGrid(mandatoryTimeSteps.begin(), mandatoryTimeSteps.end(),
    				  maturity*tGridPerYear);

    const Array x(spotMesher->locations().begin(),
    			  spotMesher->locations().end());
    const Array v(varianceMesher->locations().begin(),
    			  varianceMesher->locations().end());
    const Array tMesh(timeGrid.begin()+1, timeGrid.end());
    Matrix L(x.size(), tMesh.size());

    for (Size i=0; i < x.size(); ++i) {
    	const Real l
    		= safeLocalVolAccess(localVol, eT, x[i])/std::sqrt(v0);

    	std::fill(L.row_begin(i), L.row_end(i), l);
    }
    boost::shared_ptr<Interpolation2D> leverageFct(
    	new BilinearInterpolation(tMesh.begin(), tMesh.end(),
    							  x.begin(), x.end(), L));

	const boost::shared_ptr<FdmLinearOpComposite> hestonFwdOp(
		new FdmHestonFwdOp(mesher, hestonProcess, trafoType));

    HundsdorferScheme evolver(FdmSchemeDesc::Hundsdorfer().theta,
                              FdmSchemeDesc::Hundsdorfer().mu, hestonFwdOp);

    for (Size i=1; i < tMesh.size(); ++i) {
    	const Time t = tMesh.at(i);
    	const Time dt = t - tMesh.at(i-1);

    	for (Size j=0; j < x.size(); ++j) {
    		Array pSlice(vGrid);
    		for (Size k=0; k < vGrid; ++k)
    			pSlice[k] = p[j + k*xGrid];

    		const Real pInt = DiscreteSimpsonIntegral()(v, pSlice);
    		const Real vpInt = DiscreteSimpsonIntegral()(v, v*pSlice);

    		const Real spot = std::exp(x[j]);
    		const Real scale = pInt/vpInt;
    		const Real l = (scale >= 0.0)
    			? safeLocalVolAccess(localVol, t, spot)*std::sqrt(scale)
    		    : 1.0;

    		std::cout << std::max(0.0, std::min(10.0,l)) << ", ";

    		std::fill(L.row_begin(j)+i, L.row_end(j), l);
    	}
    	std::cout << std::endl;

    	evolver.setStep(dt);
    	evolver.step(p, t);
    }
}

test_suite* FdHestonTest::suite() {
    test_suite* suite = BOOST_TEST_SUITE("Finite Difference Heston tests");
    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testFdmHestonBarrier));
    suite->add(QUANTLIB_TEST_CASE(
                         &FdHestonTest::testFdmHestonBarrierVsBlackScholes));
    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testFdmHestonAmerican));
    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testFdmHestonIkonenToivanen));
    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testFdmHestonBlackScholes));
    suite->add(QUANTLIB_TEST_CASE(
                    &FdHestonTest::testFdmHestonEuropeanWithDividends));

    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testFdmHestonConvergence));
    return suite;
}

test_suite* FdHestonTest::experimental() {
    test_suite* suite = BOOST_TEST_SUITE("Finite Difference Heston tests");
//    suite->add(QUANTLIB_TEST_CASE(
//        &FdHestonTest::testBlackScholesFokkerPlanckFwdEquation));
//    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testSquareRootZeroFlowBC));
//    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testTransformedZeroFlowBC));
//    suite->add(QUANTLIB_TEST_CASE(
//          &FdHestonTest::testSquareRootEvolveWithStationaryDensity));
//    suite->add(QUANTLIB_TEST_CASE(
//          &FdHestonTest::testSquareRootLogEvolveWithStationaryDensity));
//    suite->add(QUANTLIB_TEST_CASE(
//        &FdHestonTest::testSquareRootFokkerPlanckFwdEquation));
//    suite->add(QUANTLIB_TEST_CASE(
//        &FdHestonTest::testHestonFokkerPlanckFwdEquation));
//    suite->add(QUANTLIB_TEST_CASE(
//        &FdHestonTest::testHestonFokkerPlanckFwdEquationLogLVLeverage));
//    suite->add(QUANTLIB_TEST_CASE(
//        &FdHestonTest::testBlackScholesFokkerPlanckFwdEquationLocalVol));

    suite->add(QUANTLIB_TEST_CASE(&FdHestonTest::testLSVCalibration));


    return suite;
}
