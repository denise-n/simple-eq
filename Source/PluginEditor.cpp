#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>


void LookAndFeel::drawRotarySlider(juce::Graphics & g, int x, int y, int width, int height, float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    using namespace juce;

    // fill circle and act as slider background
    // convert slider normalised value to angle in radians

    auto bounds = Rectangle<float>(x,y, width, height);

    // g.setColour(Colour(97u, 18u, 167)); // og purple colour
    g.setColour(Colour(255u, 220u, 180u).withAlpha(0.9f)); // light apricot

    g.fillEllipse(bounds);

    g.setColour(Colour(255u, 154u, 1u));
    g.drawEllipse(bounds, 1.f);

    if ( auto* rswl = dynamic_cast<RotarySliderWithLabels*>(&slider))
    {
        auto center = bounds.getCentre();

        // rectangle that goes from center of bounding box up to 12oclock posn
        Path p;

        Rectangle<float> r;
        r.setLeft(center.getX() - 2);
        r.setRight(center.getX() + 2);
        r.setTop(bounds.getY());
        r.setBottom(center.getY() - rswl->getTextHeight() * 1.5);

        p.addRoundedRectangle(r, 2.f);
        p.addRectangle(r);

        jassert(rotaryStartAngle < rotaryEndAngle);

        auto sliderAngRad = jmap(sliderPosProportional, 0.f, 1.f, rotaryStartAngle, rotaryEndAngle);

        // apply rotation around center of the component
        p.applyTransform(AffineTransform().rotated(sliderAngRad, center.getX(), center.getY()));
        g.fillPath(p);

        g.setFont(rswl->getTextHeight());
        auto text = rswl->getDisplayString();
        auto stringWidth = g.getCurrentFont().getStringWidth(text);

        r.setSize(stringWidth + 4, rswl->getTextHeight() + 2);
        r.setCentre(bounds.getCentre());

        g.setColour(Colours::black);
        g.fillRect(r);

        g.setColour(Colours::white);
        g.drawFittedText(text, r.toNearestInt(), juce::Justification::centred, 1);
    }





}

// =========================================================
void RotarySliderWithLabels::paint(juce::Graphics& g)
{
    using namespace juce;
    auto startAng = degreesToRadians(180.f + 45.f);
    auto endAng = degreesToRadians(180.f - 45.f) + MathConstants<float>::twoPi;

    auto range = getRange();

    auto sliderBounds = getSliderBounds();

    // bounding boxes for testing component size
    // g.setColour(Colours::red);
    // g.drawRect(getLocalBounds());
    // g.setColour(Colours::yellow);
    // g.drawRect(sliderBounds);

    getLookAndFeel().drawRotarySlider(
        g,
        sliderBounds.getX(),
        sliderBounds.getY(),
        sliderBounds.getWidth(),
        sliderBounds.getHeight(),
        jmap(getValue(), range.getStart(), range.getEnd(), 0.0, 1.0), // turn slider value into a normalised value
        startAng,
        endAng,
        *this);

    auto center = sliderBounds.toFloat().getCentre();
    auto radius  = sliderBounds.getWidth() * 0.5f;

    g.setColour(Colour(144u, 238u, 144u)); // pastel green
    g.setFont(getTextHeight());

    auto numChoices = labels.size();
    for (int i = 0; i < numChoices; ++i )
    {
        auto pos = labels[i].pos;
        jassert(0.f <= pos);
        jassert(pos <= 1.f);

        auto ang = jmap(pos, 0.f, 1.f, startAng, endAng);

        auto c = center.getPointOnCircumference(radius + getTextHeight() * 0.5f, ang);

        Rectangle<float> r;
        auto str = labels[i].label;
        r.setSize(g.getCurrentFont().getStringWidth(str), getTextHeight());
        r.setCentre(c);
        r.setY(r.getY() + getTextHeight());

        g.drawFittedText(str, r.toNearestInt(), juce::Justification::centred, 1);

    }
}

juce::Rectangle<int> RotarySliderWithLabels::getSliderBounds() const
{
    // return getLocalBounds();
    auto bounds = getLocalBounds();

    auto size = juce::jmin(bounds.getWidth(), bounds.getHeight());

    size -= getTextHeight() * 2;

    juce::Rectangle<int> r;
    r.setSize(size, size);
    r.setCentre(bounds.getCentreX(), 0);
    r.setY(2);

    return r;
}

juce::String RotarySliderWithLabels::getDisplayString() const
{
   if (auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(param))
   {
       return choiceParam->getCurrentChoiceName();
   }
    juce::String str;
    bool addK = false;

    if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*>(param))
    {
        float val = getValue();
        if (val > 999.f)
        {
            val /= 1000.f;
            addK=true;
        }
        str = juce::String(val, (addK ? 2 : 0));
    } else
    {
        jassertfalse;
    }

    if (suffix.isNotEmpty())
    {
        str << " ";
        if (addK)
        {
            str << "k";
        }
        str << suffix;
    }
    return str;
}
//=============================================================================
ResponseCurveComponent::ResponseCurveComponent(SimpleEQAudioProcessor& p) :
processorRef(p),
leftChannelFifo(&processorRef.leftChannelFifo)
{
    const auto& params = processorRef.getParameters();
    for (auto param : params)
    {
        param->addListener(this);
    }

    // 48000 / 2048 = 23hz
    leftChannelFFTDataGenerator.changeOrder(FFTOrder::order2048);
    monoBuffer.setSize(1, leftChannelFFTDataGenerator.getFFTSize());

    updateChain();
    startTimerHz(60);
}

ResponseCurveComponent::~ResponseCurveComponent() {
    const auto& params = processorRef.getParameters();
    for (auto param : params)
    {
        param->removeListener(this);
    }
}

void ResponseCurveComponent::parameterValueChanged(int parameterIndex, float newValue)
{
    parametersChanged = true;
}

void ResponseCurveComponent::timerCallback()
{
    juce::AudioBuffer<float> tempIncomingBuffer;

    while (leftChannelFifo->getAudioBuffer(tempIncomingBuffer) > 0)
    {
        // send to fft data generator
        auto size = tempIncomingBuffer.getNumSamples();

        juce::FloatVectorOperations::copy(
            monoBuffer.getWritePointer(0,0),
            monoBuffer.getReadPointer(0, size),
            monoBuffer.getNumSamples()-size
            );

        juce::FloatVectorOperations::copy(
            monoBuffer.getWritePointer(0, monoBuffer.getNumSamples()-size),
            tempIncomingBuffer.getReadPointer(0,0),
            size
        );

        leftChannelFFTDataGenerator.produceFFTDataForRendering(monoBuffer, -48.f);
    }

    // if there are fft data buffers to pull, if we can pull a buffer => generate a path
    const auto fftBounds = getAnalysisArea().toFloat();
    const auto fftSize = leftChannelFFTDataGenerator.getFFTSize();
    const auto binWidth = processorRef.getSampleRate() / double(fftSize);

    while (leftChannelFFTDataGenerator.getNumAvailableFFTDataBlocks() )
    {
        std::vector<float> fftData;
        if (leftChannelFFTDataGenerator.getFFTData(fftData))
        {
            pathProducer.generatePath(fftData, fftBounds, fftSize, binWidth, -48.f);
        }
    }

    // while there are paths that can be pulled, pull as many as we because we only display the most recent path
    while (pathProducer.getNumPathsAvailable())
    {
        pathProducer.getPath(leftChannelFFTPath);
    }


    if (parametersChanged.compareAndSetBool(false, true))
    {
        // update monochain from apvts
        updateChain();

        // signal a repaint so new response curve is drawn
        // repaint();
    }
    repaint();
}

void ResponseCurveComponent::updateChain()
{
    auto chainSettings = getChainSettings(processorRef.apvts);
    auto peakCoefficients = makePeakFilter(chainSettings, processorRef.getSampleRate());
    updateCoefficients(monoChain.get<ChainPositions::Peak>().coefficients, peakCoefficients);

    auto lowCutCoefficients = makeLowCutFilter(chainSettings, processorRef.getSampleRate());
    auto highCutCoefficients = makeHighCutFilter(chainSettings, processorRef.getSampleRate());

    updateCutFilter(monoChain.get<ChainPositions::LowCut>(), lowCutCoefficients, chainSettings.lowCutSlope);
    updateCutFilter(monoChain.get<ChainPositions::HighCut>(), highCutCoefficients, chainSettings.highCutSlope);
}

void ResponseCurveComponent::paint (juce::Graphics& g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    using namespace juce;
    g.fillAll (Colours::black);

    // draw background
    g.drawImage(background, getLocalBounds().toFloat());

    auto responseArea = getAnalysisArea();

    auto w = responseArea.getWidth();

    auto& lowcut = monoChain.get<ChainPositions::LowCut>();
    auto& peak = monoChain.get<ChainPositions::Peak>();
    auto& highcut = monoChain.get<ChainPositions::HighCut>();

    auto sampleRate = processorRef.getSampleRate();

    std::vector<double> magnitudes;

    magnitudes.resize(w);
    for (int i = 0; i < w; i++)
    {
        double magnitude = 1.f;
        auto freq = juce::mapToLog10(double(i) / double(w), 20.0, 20000.0);

        if (! monoChain.isBypassed<ChainPositions::Peak>())
            magnitude *= peak.coefficients->getMagnitudeForFrequency(freq, sampleRate);

        if (! lowcut.isBypassed<0>())
            magnitude *= lowcut.get<0>().coefficients->getMagnitudeForFrequency(freq, sampleRate);
        if (! lowcut.isBypassed<1>())
            magnitude *= lowcut.get<1>().coefficients->getMagnitudeForFrequency(freq, sampleRate);
        if (! lowcut.isBypassed<2>())
            magnitude *= lowcut.get<2>().coefficients->getMagnitudeForFrequency(freq, sampleRate);
        if (! lowcut.isBypassed<3>())
            magnitude *= lowcut.get<3>().coefficients->getMagnitudeForFrequency(freq, sampleRate);

        if (! highcut.isBypassed<0>())
            magnitude *= highcut.get<0>().coefficients->getMagnitudeForFrequency(freq, sampleRate);
        if (! highcut.isBypassed<1>())
            magnitude *= highcut.get<1>().coefficients->getMagnitudeForFrequency(freq, sampleRate);
        if (! highcut.isBypassed<2>())
            magnitude *= highcut.get<2>().coefficients->getMagnitudeForFrequency(freq, sampleRate);
        if (! highcut.isBypassed<3>())
            magnitude *= highcut.get<3>().coefficients->getMagnitudeForFrequency(freq, sampleRate);

        magnitudes[i] = Decibels::gainToDecibels(magnitude);
    }

    Path responseCurve;

    const double outputMin = responseArea.getBottom();
    const double outputMax = responseArea.getY();
    auto map = [outputMin, outputMax](double input)
    {
        return jmap(input, -24.0, 24.0, outputMin, outputMax);
    };

    responseCurve.startNewSubPath(responseArea.getX(), map(magnitudes.front()));

    // create lineto for every other magnitude
    for (size_t i = 1; i < magnitudes.size(); i++)
    {
        responseCurve.lineTo(responseArea.getX() + i, map(magnitudes[i]));
    }

    g.setColour(Colours::cornflowerblue);
    g.strokePath(leftChannelFFTPath, PathStrokeType(1));

    g.setColour(Colours::orange);
    g.drawRoundedRectangle(getRenderArea().toFloat(), 4.f, 1.f);
    g.setColour(Colours::white);
    g.strokePath(responseCurve, PathStrokeType(2.f));
}

void ResponseCurveComponent::resized()
{
    using namespace juce;
    // create new background image
    background = Image(Image::PixelFormat::RGB, getWidth(), getHeight(), true);

    // create graphics context
    Graphics g(background);

    // draw frequency lines
    Array<float> freqs {
        20, 50, 100,
        200, 500 ,1000,
        2000, 5000, 10000,
        20000
    };

    // cache info about analysis area
    auto  renderArea = getAnalysisArea();
    auto left = renderArea.getX();
    auto right = renderArea.getRight();
    auto top = renderArea.getY();
    auto bottom = renderArea.getBottom();
    auto width = renderArea.getWidth();

    // cache value into an array
    Array<float> xs;
    for ( auto f : freqs)
    {
        auto normX = mapFromLog10(f, 20.f, 20000.f);
        xs.add(left + width * normX);
    }



    g.setColour(Colours::dimgrey);
    for (auto x : xs)
    {
        g.drawVerticalLine(x, top, bottom);
    }

    // draw gain lines
    Array<float> gain {
        -24, -12, 0, 12, 24
    };

    for (auto gDb : gain)
    {
        auto y = jmap(gDb, -24.f, 24.f, float(bottom), float(top));
        g.setColour(gDb == 0.f ? (Colour(144u, 238u, 144u)) : Colours::darkgrey); // add green center line where db=0
        g.drawHorizontalLine(y, left, right);
    }

    g.setColour(Colours::lightgrey);
    const int fontHeight = 10;
    g.setFont(fontHeight);

    // draw freq text above grid at particular position
    for (int i = 0; i < freqs.size(); i++)
    {
        auto f = freqs[i];
        auto x = xs[i];

        bool addK = false;
        String str;
        if ( f > 999.f )
        {
            addK = true;
            f /= 1000.f;
        }

        str << f;
        if (addK)
        {
            str << "k";
        }
        str << "Hz";

        auto textWidth = g.getCurrentFont().getStringWidth(str);

        Rectangle<int> r;
        r.setSize(textWidth, fontHeight);
        r.setCentre(x, 0);
        r.setY(1);

        g.drawFittedText(str, r, juce::Justification::centred, 1);
    }

    // add gain labels
    for ( auto gDb : gain)
    {
        auto y = jmap(gDb, -24.f, 24.f, float(bottom), float(top));
        String str;
        if (gDb > 0)
        {
            str << "+";
        }
        str << gDb;
        auto textWidth = g.getCurrentFont().getStringWidth(str);

        Rectangle<int> r;
        r.setSize(textWidth, fontHeight);
        r.setX(getWidth() - textWidth); // draw on the right side of the screen
        r.setCentre(r.getCentreX(), y);

        g.setColour(gDb == 0.f ? Colour(144u, 238u, 144u) : Colours::lightgrey);

        g.drawFittedText(str, r, juce::Justification::centred, 1);

        // analyser scale
        str.clear();
        str << (gDb - 24.f);

        r.setX(1); // change position to far left
        textWidth = g.getCurrentFont().getStringWidth(str);
        r.setSize(textWidth, fontHeight);
        g.setColour(Colours::lightgrey);
        g.drawFittedText(str, r, juce::Justification::centred, 1);

    }


}

juce::Rectangle<int> ResponseCurveComponent::getRenderArea()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(12);
    bounds.removeFromBottom(2);
    bounds.removeFromLeft(20);
    bounds.removeFromRight(20);

    return bounds;
}

juce::Rectangle<int> ResponseCurveComponent::getAnalysisArea()
{
    auto bounds = getRenderArea();
    bounds.removeFromTop(4);
    bounds.removeFromBottom(4);
    return bounds;
}



//==============================================================================
SimpleEQAudioProcessorEditor::SimpleEQAudioProcessorEditor (SimpleEQAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
peakFreqSlider(*processorRef.apvts.getParameter("Peak Freq"), "Hz"),
peakGainSlider(*processorRef.apvts.getParameter("Peak Gain"), "dB"),
peakQualitySlider(*processorRef.apvts.getParameter("Peak Quality"), ""),
lowCutFreqSlider(*processorRef.apvts.getParameter("LowCut Freq"), "Hz"),
highCutFreqSlider(*processorRef.apvts.getParameter("HighCut Freq"), "Hz"),
lowCutSlopeSlider(*processorRef.apvts.getParameter("LowCut Slope"), "dB/Oct"),
highCutSlopeSlider(*processorRef.apvts.getParameter("HighCut Slope"), "dB/Oct"),
responseCurveComponent(processorRef),
peakFreqSliderAttachment(processorRef.apvts, "Peak Freq", peakFreqSlider),
peakGainSliderAttachment(processorRef.apvts, "Peak Gain", peakGainSlider),
peakQualitySliderAttachment(processorRef.apvts, "Peak Quality", peakQualitySlider),
lowCutFreqSliderAttachment(processorRef.apvts, "LowCut Freq", lowCutFreqSlider),
highCutFreqSliderAttachment(processorRef.apvts, "HighCut Freq", highCutFreqSlider),
lowCutSlopeSliderAttachment(processorRef.apvts, "LowCut Slope", lowCutSlopeSlider),
highCutSlopeSliderAttachment(processorRef.apvts, "HighCut Slope", highCutSlopeSlider)
{
    // Make sure that before the constructor has finished, you've set the
    // editor's size to whatever you need it to be.

    peakFreqSlider.labels.add({0.f, "20Hz"});
    peakFreqSlider.labels.add({1.f, "20kHz"});

    peakGainSlider.labels.add({0.f, "-24dB"});
    peakGainSlider.labels.add({1.f, "+24dB"});

    peakQualitySlider.labels.add({0.f, "0.1"});
    peakQualitySlider.labels.add({1.f, "10.0"});

    lowCutFreqSlider.labels.add({0.f, "20Hz"});
    lowCutFreqSlider.labels.add({1.f, "20kHz"});

    highCutFreqSlider.labels.add({0.f, "20Hz"});
    highCutFreqSlider.labels.add({1.f, "20kHz"});

    lowCutSlopeSlider.labels.add({0.f, "12"});
    lowCutSlopeSlider.labels.add({1.f, "48"});

    highCutSlopeSlider.labels.add({0.f, "12"});
    highCutSlopeSlider.labels.add({1.f, "48"});


    for ( auto* comp: getComps())
    {
        addAndMakeVisible(comp);

        setSize (600, 400);
    }
}

    SimpleEQAudioProcessorEditor::~SimpleEQAudioProcessorEditor()
    {
    }

    //==============================================================================
    void SimpleEQAudioProcessorEditor::paint (juce::Graphics& g)
    {
        // (Our component is opaque, so we must completely fill the background with a solid colour)
        using namespace juce;
        g.fillAll (Colours::black);
    }

    void SimpleEQAudioProcessorEditor::resized()
    {
        // This is generally where you'll want to lay out the positions of any
        // subcomponents in your editor..

        // Top 1/3 of display for showing freq responses
        // Bottom 2/3 is sliders
        auto bounds = getLocalBounds();
        float hRatio = 25.f/ 100.f; //JUCE_LIVE_CONSTANT(33) / 100.f;
        auto responseArea = bounds.removeFromTop(bounds.getHeight() * hRatio);

        responseCurveComponent.setBounds(responseArea);

        bounds.removeFromTop(5);

        auto lowCutArea = bounds.removeFromLeft(bounds.getWidth() * 0.33);
        auto highCutArea = bounds.removeFromRight(bounds.getWidth() * 0.5);

        lowCutFreqSlider.setBounds(lowCutArea.removeFromTop(lowCutArea.getHeight() * 0.5));
        lowCutSlopeSlider.setBounds(lowCutArea);

        highCutFreqSlider.setBounds(highCutArea.removeFromTop(highCutArea.getHeight() * 0.5));
        highCutSlopeSlider.setBounds(highCutArea);

        peakFreqSlider.setBounds(bounds.removeFromTop(bounds.getHeight() * 0.33));
        peakGainSlider.setBounds(bounds.removeFromTop(bounds.getHeight() * 0.5));
        peakQualitySlider.setBounds(bounds);
    }

    std::vector<juce::Component*> SimpleEQAudioProcessorEditor::getComps()
    {
        return {
            &peakFreqSlider,
            &peakGainSlider,
            &peakQualitySlider,
            &lowCutFreqSlider,
            &highCutFreqSlider,
            &lowCutSlopeSlider,
            &highCutSlopeSlider,
            &responseCurveComponent
        };
    }
